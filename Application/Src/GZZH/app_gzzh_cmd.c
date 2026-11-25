/**
 * @file    app_gzzh_cmd.c
 * @brief   GZZH 命令处理实现：故障查询 / 文字显示 / 清屏 / 设置 IP / 全屏纯色
 *
 * 现场行为以参考实现（`netHandler` 的五个分支）为准；文档没写、或与参考实现
 * 冲突的地方，逐条在对应函数上方注明取舍。
 */

#include "app_gzzh_cmd.h"

#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "app_render.h"
#include "app_screen.h"

/* ---- 字体名：GBK 两字恰好 4 字节（UTF-8 源文件里的"宋体"是 6 字节，不能直接拿去比） ----
 * 只保留"宋体"一份：参考实现里第二个 `memcmp` 分支不可达（见 _gzzh_map_font_type）。 */
static const uint8_t s_gzzh_font_song[4] = {0xCB, 0xCE, 0xCC, 0xE5}; /**< "宋体" */

/** @brief 0x10 的应答内容：**恒无故障**
 *
 *  本机没有温度 / 光敏 / 防雷 / 220V / 单像素检测，16 位故障表没有任何来源，
 *  按"硬件无此故障信息该位补 0"的文档要求回全 0。
 *  位定义（参考实现 `ErrorInfo_TypeDef`）：bit1 控制器、bit2 显示模组、
 *  bit4 单像素管、bit5 检测系统、bit6 输入 220V、bit7 防雷器、bit8 光敏、bit9 温度。
 *
 *  **不能加 `const`**：UDP 发送是"引用调用方缓冲"（`netbuf_ref`），ETH 的 TX DMA
 *  只读得到片内 SRAM —— 缓冲一旦落在 Flash（`static const`）就 DMA 出错：描述符
 *  OWN 位不释放 → 下次发送永远 BUSY → `pl_eth.c` 的重试循环把 tcpip 线程卡死，
 *  整条网（含 ARP）停摆。实测过一次，就是 2 字节应答放在 Flash 里引起的。
 *  同一条规矩适用于所有"回包/发包"缓冲：**必须是片内 SRAM（.data/.bss/栈），
 *  不能是 Flash（const 表）或 CCMRAM**。 */
static uint8_t s_gzzh_no_fault[GZZH_STATUS_BYTES] = {0x00, 0x00};

static void _gzzh_cmd_status(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len);
static void _gzzh_cmd_text(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len);
static void _gzzh_cmd_clear(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len);
static void _gzzh_cmd_set_ip(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len);
static void _gzzh_cmd_fill(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len);

/* 命令表：按命令字查找，顺序无关；新增命令 = 加一行 + 把 GZZH_CMD_CNT 加一 */
const app_gzzh_cmd_entry_t g_gzzh_cmd_table[] = {
    {.cmd = APP_GZZH_CMD_TYPE_STATUS, .handler_fn = _gzzh_cmd_status},
    {.cmd = APP_GZZH_CMD_TYPE_TEXT, .handler_fn = _gzzh_cmd_text},
    {.cmd = APP_GZZH_CMD_TYPE_CLEAR, .handler_fn = _gzzh_cmd_clear},
    {.cmd = APP_GZZH_CMD_TYPE_SET_IP, .handler_fn = _gzzh_cmd_set_ip},
    {.cmd = APP_GZZH_CMD_TYPE_FILL, .handler_fn = _gzzh_cmd_fill},
};

static_assert(sizeof(g_gzzh_cmd_table) / sizeof(g_gzzh_cmd_table[0]) == GZZH_CMD_CNT,
              "命令表项数与 GZZH_CMD_CNT 不一致");

/* ================================================================
 *  数值域映射
 * ================================================================ */

/** @brief 线上 RGB → 面板色：**R 优先，其次 G，最后 B**（非 0 即该通道亮）
 *
 *  这是参考实现的口径，**不按组合取色**：红+绿会得到红，不是黄。
 *  参考实现那块屏的颜色枚举里有蓝；本工程面板没有蓝灯时由
 *  `supported_color_mask` 掩成黑色 —— "位与即物理现实"是本工程既定语义，
 *  不在这里做"最近似色"降级。
 *  @param rgb 3 字节 R/G/B
 *  @return 面板颜色枚举 */
static dev_display_color_t _gzzh_map_color(const uint8_t rgb[3])
{
    if (rgb[0]) return DEV_DISPLAY_COLOR_RED;
    if (rgb[1]) return DEV_DISPLAY_COLOR_GREEN;
    if (rgb[2]) return DEV_DISPLAY_COLOR_BLUE;
    return DEV_DISPLAY_COLOR_BLACK;
}

/** @brief 4 字节 GBK 字体名 → 字型：**照抄参考实现的判断（含它的倒置）**
 *
 *  **线上口径以参考实现为准**（它是现场跑过的代码）。参考实现把三个 `memcmp` 的返回
 *  值直接当条件用（`if (memcmp(...))` 在**不相等**时成立），于是实际行为是：
 *    · `"宋体"` → **黑体**
 *    · 其余（`"黑体"` / `"楷体"` / `"仿宋"` / 未知）→ **宋体**
 *  `"楷体"` 那一支在参考实现里不可达（宋体已在第一支命中，其余都落第一支的宋体）。
 *
 *  **不要"顺手修好"**：那会让本机显示的字型与现场已验证的屏不一致。真要改口径，
 *  先确认上位机发的字体名与期望，再同时改这里与 `test_gzzh.c` 的 `case_text_font_map`。
 *  @param name 4 字节 GBK 字体名
 *  @return 本工程字型枚举 */
static app_font_type_t _gzzh_map_font_type(const uint8_t name[4])
{
    if (0 == memcmp(name, s_gzzh_font_song, sizeof(s_gzzh_font_song))) return APP_FONT_TYPE_HT;
    return APP_FONT_TYPE_ST;
}

/** @brief 字号码 → 本工程字号
 *
 *  文档写"字体大小 2字节 例如 32*32"，按**像素高**收（取首字节）。参考实现
 *  `switch (font_size[0])` 只认 `font16/24/32/48` 四档（14/20 也落在 default→16），
 *  所以这里同样只认 **16/24/32/48**；另外兼容"0~3 序号"那一种读法
 *  （0=16 / 1=24 / 2=32 / 3=48，因为参考实现的常量数值没随源码给出）。
 *  都不匹配时按 16 号显示并打日志 —— 静默按别的字号显示会让现场"字很小"却查不出原因。
 *  @param code 字号码（载荷首字节）
 *  @return 本工程字号枚举 */
static app_font_size_t _gzzh_map_font_size(uint8_t code)
{
    switch (code) {
        case 16: return APP_FONT_SIZE_16;
        case 24: return APP_FONT_SIZE_24;
        case 32: return APP_FONT_SIZE_32;
        case 48: return APP_FONT_SIZE_48;

        case 0: return APP_FONT_SIZE_16;
        case 1: return APP_FONT_SIZE_24;
        case 2: return APP_FONT_SIZE_32;
        case 3: return APP_FONT_SIZE_48;

        default:
            printf("[gzzh] 字号码 %u 不认识（既非像素高也非 0~3 序号），按 16 号显示\n",
                   (unsigned)code);
            return APP_FONT_SIZE_16;
    }
}

/** @brief 把整屏填成指定颜色（0x20 的"整屏替换"与 0x30 清屏共用）
 *  @param color 目标颜色
 *  @param persist 这一帧是否落盘（清屏/纯黑不落盘） */
static void _gzzh_fill_screen(dev_display_color_t color, bool persist)
{
    app_render(&(app_render_cfg_t){
        .type    = APP_RENDER_TYPE_FILL,
        .x       = 0,
        .y       = 0,
        .w       = app_screen_rows(),
        .h       = app_screen_cols(),
        .color   = color,
        .persist = persist,
    });
}

/* ================================================================
 *  命令实现
 * ================================================================ */

/** @brief 0x10 故障查询：回**裸 2 字节**（无帧头帧尾）
 *
 *  文档写"应答 2 个字节状态标志（基本数据包部分的内容）"，读起来像完整帧；
 *  参考实现是 `UDP_DataSend(rtn_buff, 2)`，即裸 2 字节。现场以参考实现为准。 */
static void _gzzh_cmd_status(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len)
{
    (void)payload;
    (void)payload_len;

    GZZH_TRACE_PRINTF("[gzzh] 0x10 故障查询 -> 回裸 %u 字节 00 00\n", (unsigned)GZZH_STATUS_BYTES);

    if (app_ccb_send(ccb, s_gzzh_no_fault, (uint16_t)sizeof(s_gzzh_no_fault)) < 0)
        printf("[gzzh] 故障应答发送失败（通道不可用）\n");
}

/** @brief 0x20 立即显示文字：整屏替换 + 从 (x,y) 起按字号排版
 *
 *  **先清整屏再画**：文档的 0x30 是独立清屏命令，但上位机换一条更短的字时不发
 *  0x30，不清屏就会新旧叠字（与 CDSTD A1 同口径）。清屏在坐标门禁**之后** ——
 *  一条坐标越界的命令不该擦掉现有画面。
 *
 *  **裁剪而不是缩放**：帧里的"屏宽/屏高"是主机视角，本机一律用自己的逻辑屏几何。
 *  x/y 越界整帧丢弃；文本超宽由 `app_render` 按字形截断（`word_wrap = false`，
 *  对应文档"超出每行的显示上限，自动截断舍去显示"）；`\n` 逐行下移一个字号。 */
static void _gzzh_cmd_text(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len)
{
    (void)ccb; /* 文字显示协议未定义应答帧 */

    if (payload_len <= sizeof(app_gzzh_text_t)) {
        printf("[gzzh] 文字显示载荷 %u B 不足（固定头 %u B + 至少 1 B 文本），整帧丢弃\n",
               (unsigned)payload_len, (unsigned)sizeof(app_gzzh_text_t));
        return;
    }

    const app_gzzh_text_t *text_ctx = (const app_gzzh_text_t *)payload;
    const uint16_t text_len         = (uint16_t)(payload_len - sizeof(app_gzzh_text_t));
    const uint16_t x     = (uint16_t)((uint16_t)text_ctx->x[0] | ((uint16_t)text_ctx->x[1] << 8));
    const uint16_t y     = (uint16_t)((uint16_t)text_ctx->y[0] | ((uint16_t)text_ctx->y[1] << 8));
    const uint16_t rows  = app_screen_rows();
    const uint16_t cols  = app_screen_cols();

    if ((x >= rows) || (y >= cols)) {
        printf("[gzzh] 文字坐标 (%u,%u) 超出逻辑屏 %ux%u，整帧丢弃\n", (unsigned)x, (unsigned)y,
               (unsigned)rows, (unsigned)cols);
        return;
    }

    const dev_display_color_t color = _gzzh_map_color(text_ctx->color);
    const app_font_size_t size       = _gzzh_map_font_size(text_ctx->font_size[0]);
    const app_font_type_t type       = _gzzh_map_font_type(text_ctx->font_name);

#if GZZH_TRACE
    /* 原始字段 + 解析结果一起打：字型/字号/颜色三处都有"看起来对、其实映射错"的风险 */
    char hex[3U * 16U + 1U];
    const uint16_t dump_len = (text_len < 16U) ? text_len : 16U;
    static const char k_hex_digits[] = "0123456789ABCDEF";
    for (uint16_t i = 0; i < dump_len; i++) {
        hex[i * 3U + 0U] = k_hex_digits[(text_ctx->text[i] >> 4) & 0x0FU];
        hex[i * 3U + 1U] = k_hex_digits[text_ctx->text[i] & 0x0FU];
        hex[i * 3U + 2U] = (uint16_t)(i + 1U < dump_len) ? ' ' : '\0';
    }
    if (dump_len == 0U) hex[0] = '\0';

    GZZH_TRACE_PRINTF("[gzzh] 0x20 x=%u y=%u font=%02X%02X%02X%02X size=%02X,%02X "
                      "rgb=%02X,%02X,%02X text=%uB\n",
                      (unsigned)x, (unsigned)y, (unsigned)text_ctx->font_name[0],
                      (unsigned)text_ctx->font_name[1], (unsigned)text_ctx->font_name[2],
                      (unsigned)text_ctx->font_name[3], (unsigned)text_ctx->font_size[0],
                      (unsigned)text_ctx->font_size[1], (unsigned)text_ctx->color[0],
                      (unsigned)text_ctx->color[1], (unsigned)text_ctx->color[2],
                      (unsigned)text_len);
    GZZH_TRACE_PRINTF("[gzzh] 0x20 -> 字型=%u 字号=%u 色=%u persist=%u | %s%s\n", (unsigned)type,
                      (unsigned)size, (unsigned)color,
                      (unsigned)(color != DEV_DISPLAY_COLOR_BLACK), hex,
                      (text_len > dump_len) ? " ..." : "");
#endif

    /* 纯黑文本等于"画了看不见的东西"，结果就是一块黑屏 —— 按清屏口径不落盘 */
    _gzzh_fill_screen(DEV_DISPLAY_COLOR_BLACK, false);

    app_render(&(app_render_cfg_t){
        .type   = APP_RENDER_TYPE_TEXT,
        .x      = x,
        .y      = y,
        .w      = (uint16_t)(rows - x), /* 区域 = 从 x 到屏右缘 */
        .h      = (uint16_t)(cols - y),
        .style  = &(app_render_style_t){
            .h_align = APP_RENDER_ALIGN_LEFT_UP,
            .v_align = APP_RENDER_ALIGN_LEFT_UP,
        },
        .color     = color,
        .persist   = (color != DEV_DISPLAY_COLOR_BLACK),
        .font_size = size,
        .font_type = type,
        .text_enc  = APP_FONT_ENC_GBK,
        .text      = (const char *)text_ctx->text,
        .len       = text_len,
    });
}

/** @brief 0x30 清屏：整屏填黑，**不落盘**
 *
 *  清屏不落盘是本工程通则（`app_render_cfg_t.persist` 的说明）：交通屏上电要有
 *  内容可判屏，把清屏存下去会让掉电重启后黑屏。 */
static void _gzzh_cmd_clear(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len)
{
    (void)ccb;
    (void)payload;
    (void)payload_len;

    GZZH_TRACE_PRINTF("[gzzh] 0x30 清屏（整屏填黑，不落盘）\n");

    _gzzh_fill_screen(DEV_DISPLAY_COLOR_BLACK, false);
}

/** @brief 0x80 全屏纯色显示（文档未列，参考实现有）：3 字节 RGB 铺满整屏
 *
 *  参考实现把它当"测试不保存"用；本工程按"显示命令落盘、纯黑不落盘"处理 ——
 *  纯黑与清屏等价，存下去同样会导致掉电黑屏。 */
static void _gzzh_cmd_fill(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len)
{
    (void)ccb;

    if (payload_len < GZZH_FILL_BYTES) {
        printf("[gzzh] 全屏纯色载荷 %u B < %u B，整帧丢弃\n", (unsigned)payload_len,
               (unsigned)GZZH_FILL_BYTES);
        return;
    }

    const dev_display_color_t color = _gzzh_map_color(payload);

    GZZH_TRACE_PRINTF("[gzzh] 0x80 rgb=%02X,%02X,%02X -> 色=%u persist=%u\n", (unsigned)payload[0],
                      (unsigned)payload[1], (unsigned)payload[2], (unsigned)color,
                      (unsigned)(color != DEV_DISPLAY_COLOR_BLACK));

    _gzzh_fill_screen(color, (color != DEV_DISPLAY_COLOR_BLACK));
}

/** @brief 0x40 设置 IP：写入 IAP 记录并回 0x50 应答
 *
 *  **只落盘、不在运行态生效**：运行中换 IP 会把当前连接连同这条命令的应答一起断掉，
 *  新地址下次上电生效 —— 与 RLS 的 `IP`、LDI 的 06H 同口径。
 *
 *  载荷带端口（参考实现有、文档没有），端口是**大端**（参考实现
 *  `port[1] | port[0] << 8`）—— 本协议其余多字节字段都是小端，这处是唯一例外。
 *
 *  应答帧：同引导串 + 包序号全 0 + 长度 31 + 保留 0 + 命令字 0x50 + 载荷原样回 + 结束符。 */
static void _gzzh_cmd_set_ip(app_ccb_t *ccb, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < sizeof(app_gzzh_net_cfg_t)) {
        printf("[gzzh] 设置 IP 载荷 %u B < %u B，整帧丢弃\n", (unsigned)payload_len,
               (unsigned)sizeof(app_gzzh_net_cfg_t));
        return;
    }

    const app_gzzh_net_cfg_t *net_cfg = (const app_gzzh_net_cfg_t *)payload;
    const uint32_t port = ((uint32_t)net_cfg->port[0] << 8) | (uint32_t)net_cfg->port[1];

    GZZH_TRACE_PRINTF("[gzzh] 0x40 ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u port=%u "
                      "-> 回 0x50 %u 字节\n",
                      (unsigned)net_cfg->ip[0], (unsigned)net_cfg->ip[1], (unsigned)net_cfg->ip[2],
                      (unsigned)net_cfg->ip[3], (unsigned)net_cfg->mask[0],
                      (unsigned)net_cfg->mask[1], (unsigned)net_cfg->mask[2],
                      (unsigned)net_cfg->mask[3], (unsigned)net_cfg->gw[0], (unsigned)net_cfg->gw[1],
                      (unsigned)net_cfg->gw[2], (unsigned)net_cfg->gw[3], (unsigned)port,
                      (unsigned)GZZH_REPORT_IP_BYTES);

    app_flash_iap_update_net_cfg(net_cfg->ip, net_cfg->mask, net_cfg->gw, port);

    printf("[gzzh] 设置 IP %u.%u.%u.%u / %u.%u.%u.%u / %u.%u.%u.%u 端口 %u：已写入记录，重启后生效\n",
           (unsigned)net_cfg->ip[0], (unsigned)net_cfg->ip[1], (unsigned)net_cfg->ip[2],
           (unsigned)net_cfg->ip[3], (unsigned)net_cfg->mask[0], (unsigned)net_cfg->mask[1],
           (unsigned)net_cfg->mask[2], (unsigned)net_cfg->mask[3], (unsigned)net_cfg->gw[0],
           (unsigned)net_cfg->gw[1], (unsigned)net_cfg->gw[2], (unsigned)net_cfg->gw[3],
           (unsigned)port);

    uint8_t frame[GZZH_REPORT_IP_BYTES] = {0};
    app_gzzh_frame_t *reply             = (app_gzzh_frame_t *)frame;

    memcpy(reply->head, g_gzzh_head, sizeof(g_gzzh_head));
    /* 包序号、保留位恒 0（静态数组已清零） */
    reply->length[0] = (uint8_t)(GZZH_REPORT_IP_BYTES & 0xFFU);
    reply->length[1] = (uint8_t)((GZZH_REPORT_IP_BYTES >> 8) & 0xFFU);
    reply->cmd[0]    = (uint8_t)APP_GZZH_CMD_TYPE_REPORT_IP;
    memcpy(reply->payload, net_cfg, sizeof(*net_cfg));
    frame[GZZH_REPORT_IP_BYTES - 1] = GZZH_TAIL_BYTE;

    if (app_ccb_send(ccb, frame, (uint16_t)sizeof(frame)) < 0)
        printf("[gzzh] 设置 IP 应答发送失败（通道不可用）\n");
}
