/**
 * @file    app_p25rls_cmd.c
 * @brief   P25RLS 命令处理实现：全屏图片(SV) / 设置 IP(IP) / 查询 IP(SC) /
 *          全屏纯色(FS) / 文字(HZ) / 清屏(CL) / 波特率(BD)
 *
 * 口径以协议文档为准，文档没写的按"与 RLS 同族的既有做法"处理，逐条在各函数上方注明。
 */

#include "app_p25rls_cmd.h"

#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_p25rls_pic.h"
#include "app_render.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "bcc_utils.h"
#include "pl_net.h"

static void _p25rls_cmd_display_save(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_set_ip(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_aquire_ip(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_fill(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_text(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_clear(app_ccb_t *ccb, const void *payload, uint16_t payload_len);
static void _p25rls_cmd_baud(app_ccb_t *ccb, const void *payload, uint16_t payload_len);

/* 命令表：按命令字查找，顺序无关；新增命令 = 加一行 + 把 P25RLS_CMD_CNT 加一 */
const app_p25rls_cmd_entry_t g_p25rls_cmd_table[] = {
    {.cmd = APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, .handler_fn = _p25rls_cmd_display_save},
    {.cmd = APP_P25RLS_CMD_TYPE_SET_IP, .handler_fn = _p25rls_cmd_set_ip},
    {.cmd = APP_P25RLS_CMD_TYPE_AQUIRY_IP, .handler_fn = _p25rls_cmd_aquire_ip},
    {.cmd = APP_P25RLS_CMD_TYPE_FILL, .handler_fn = _p25rls_cmd_fill},
    {.cmd = APP_P25RLS_CMD_TYPE_TEXT, .handler_fn = _p25rls_cmd_text},
    {.cmd = APP_P25RLS_CMD_TYPE_CLEAR, .handler_fn = _p25rls_cmd_clear},
    {.cmd = APP_P25RLS_CMD_TYPE_BAUD, .handler_fn = _p25rls_cmd_baud},
};

static_assert(sizeof(g_p25rls_cmd_table) / sizeof(g_p25rls_cmd_table[0]) == P25RLS_CMD_CNT,
              "命令表项数与 P25RLS_CMD_CNT 不一致");

/* ================================================================
 *  数值域映射
 * ================================================================ */

/** @brief 线上颜色码 → 面板颜色：1 红 / 2 绿 / 3 黄；其余值返回 false
 *
 *  **不能把线上值直接 cast 成 `dev_display_color_t`**：本协议是 1/2/3 = 红/绿/黄，
 *  而面板枚举是 1 红 / 2 绿 / 3 黄 —— 恰好同序，但只有这三个值合法；4 以上
 *  直接透传会跑到扫描驱动的 BSRR 查表之外（RLS 那边有详细说明）。
 *  @param code 线上颜色码
 *  @param[out] out 接收面板颜色；返回 false 时不写
 *  @return true = 合法 */
static bool _p25rls_map_color(uint8_t code, dev_display_color_t *out)
{
    switch (code) {
        case 1: *out = DEV_DISPLAY_COLOR_RED; return true;
        case 2: *out = DEV_DISPLAY_COLOR_GREEN; return true;
        case 3: *out = DEV_DISPLAY_COLOR_YELLOW; return true;
        default: return false;
    }
}

/** @brief 点阵大小码 → 字号：0 = 16 / 1 = 24 / 2 = 32
 *
 *  协议只有这三档、没有字型字段，所以字型固定宋体。超出范围回落 16 并打日志
 *  （静默按别的字号显示会让现场"字很小"却查不出原因）。
 *  @param code 点阵大小码
 *  @return 本工程字号枚举 */
static app_font_size_t _p25rls_map_dot_size(uint8_t code)
{
    switch (code) {
        case 0: return APP_FONT_SIZE_16;
        case 1: return APP_FONT_SIZE_24;
        case 2: return APP_FONT_SIZE_32;
        default:
            printf("[p25rls] 点阵大小码 %u 不认识（只认 0/1/2），按 16 点阵显示\n", (unsigned)code);
            return APP_FONT_SIZE_16;
    }
}

/** @brief 应用显示命令的亮度字节（协议：0~31，0 最暗 / 31 最亮；255 = 自动）
 *
 *  与 RLS 同口径：面板只有 3 位亮度（0~7），0~31 线性压到 **1~7**（0 落 1，
 *  因为 0 在硬件上是"不亮"，而文档里 0 是"最暗"，关屏另论）。
 *  @param level 线上亮度值 */
static void _p25rls_apply_light_level(uint8_t level)
{
    if (level == 255U) {
        app_light_sensor_resume();
        return;
    }
    if (level > 31U) level = 31U;

    const uint8_t hw = (uint8_t)(1U + (uint16_t)((uint16_t)level * 6U + 15U) / 31U);
    app_light_sensor_set_fixed(hw);
}

/** @brief 整屏填成指定颜色（清屏 / 纯色共用）
 *  @param color   目标颜色
 *  @param persist 本帧是否交由渲染层落盘（P25 只有 SV 走槽记录，这里恒 false） */
static void _p25rls_fill_screen(dev_display_color_t color, bool persist)
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

/** @brief `SV` 全屏图片显示并保存：亮度 + 图片编号 + 双平面 1bpp 位图
 *
 *  图片编号即槽号（0/1/2），越界整帧丢弃（不动当前画面）。
 *  **先去存槽、再显示**：显示读的是槽（与上电重放同一条路径），存失败只影响掉电保持。
 *  落盘标志不走渲染层的 persist（那份记录只有单色），见 app_p25rls_pic.h。 */
static void _p25rls_cmd_display_save(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb; /* 协议没有应答 */

    if (payload_len < P25RLS_DISPLAY_PAYLOAD_BYTES) {
        printf("[p25rls] SV 载荷 %u B < %u B（亮度+图片号+%uB 位图），整帧丢弃\n",
               (unsigned)payload_len, (unsigned)P25RLS_DISPLAY_PAYLOAD_BYTES,
               (unsigned)P25RLS_BITMAP_BYTES);
        return;
    }

    const app_p25rls_display_t *ctx = (const app_p25rls_display_t *)payload;

    if (ctx->pic_num >= P25RLS_PIC_SLOT_COUNT) {
        printf("[p25rls] 图片编号 %u 超出槽数 %u，整帧丢弃\n", (unsigned)ctx->pic_num,
               (unsigned)P25RLS_PIC_SLOT_COUNT);
        return;
    }

    _p25rls_apply_light_level(ctx->brightness);

    (void)p25rls_pic_save(ctx->pic_num, ctx->bitmap);
    p25rls_pic_show(ctx->pic_num);
}

/** @brief `IP` 修改 IP：DATA = IP[4] | 掩码[4] | 网关[4]（整帧 21 字节）
 *
 *  **只写 IAP 记录、不在运行态生效** —— 与 RLS 的 IP、IAP 0AH 同口径：运行中换 IP
 *  会把当前连接连同这条命令的后果一起断掉，新地址下次上电生效。
 *  协议帧里**没有端口字段**：端口沿用设备当前配置（记录里取，没有再回落运行态）。
 *  协议未定义应答帧，本函数不回包。 */
static void _p25rls_cmd_set_ip(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb;

    if (payload_len < P25RLS_NET_CFG_BYTES) {
        printf("[p25rls] IP 载荷 %u B < %u B，整帧丢弃\n", (unsigned)payload_len,
               (unsigned)P25RLS_NET_CFG_BYTES);
        return;
    }

    const app_p25rls_net_cfg_t *cfg = (const app_p25rls_net_cfg_t *)payload;

    uint32_t port = app_tcp_server_get_port();
    app_flash_iap_net_cfg_t rec;
    if (app_iap_get_net_cfg(&rec)) port = rec.port;

    app_flash_iap_update_net_cfg(cfg->ip, cfg->mask, cfg->gw, port);

    printf("[p25rls] 设置 IP %u.%u.%u.%u / %u.%u.%u.%u / %u.%u.%u.%u：已写入记录，重启后生效\n",
           (unsigned)cfg->ip[0], (unsigned)cfg->ip[1], (unsigned)cfg->ip[2], (unsigned)cfg->ip[3],
           (unsigned)cfg->mask[0], (unsigned)cfg->mask[1], (unsigned)cfg->mask[2],
           (unsigned)cfg->mask[3], (unsigned)cfg->gw[0], (unsigned)cfg->gw[1],
           (unsigned)cfg->gw[2], (unsigned)cfg->gw[3]);
}

/** @brief `SC` 查询 IP：回一帧 `RB`，携带本机**当前生效**的 IP/掩码/网关
 *
 *  查询里的地址字段不参与应答（报文语义是"谁在线、报出你的地址"），回运行态地址
 *  而不是记录里的旧值。BCC 按文档正文："帧头到 BCC 字节之前所有字节的异或"
 *  → 覆盖前 18 字节。
 *  @param ccb 本帧来源通道 */
static void _p25rls_cmd_aquire_ip(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)payload;
    (void)payload_len;

    uint8_t frame[P25RLS_REPORT_IP_BYTES];
    uint8_t ip[4], mask[4], gw[4];

    pl_net_get_ip(ip, mask, gw);

    frame[0] = P25RLS_HEAD_BYTE0;
    frame[1] = P25RLS_HEAD_BYTE1;
    frame[2] = 0x00;
    frame[3] = (uint8_t)P25RLS_REPORT_IP_BYTES; /* 21：高字节恒 0，线上是高字节在前 */
    frame[4] = (uint8_t)(APP_P25RLS_CMD_TYPE_REPORT_IP & 0xFFU);       /* 'R' */
    frame[5] = (uint8_t)((APP_P25RLS_CMD_TYPE_REPORT_IP >> 8) & 0xFFU); /* 'B' */
    memcpy(&frame[6], ip, 4);
    memcpy(&frame[10], mask, 4);
    memcpy(&frame[14], gw, 4);
    frame[18] = bcc_calc(frame, 18U); /* 帧头 → 数据域末尾，不含 BCC 自身 */
    frame[19] = P25RLS_TAIL_BYTE0;
    frame[20] = P25RLS_TAIL_BYTE1;

    if (app_ccb_send(ccb, frame, (uint16_t)sizeof(frame)) < 0)
        printf("[p25rls] 查询应答发送失败（通道不可用）\n");
}

/** @brief `FS` 全屏纯色：DATA 单字节 1 红 / 2 绿 / 3 黄
 *
 *  文档标注"测试不保存" → **不落盘**（掉电后不恢复）。
 *  只做颜色填充，不动亮度（亮度是 SV 的字段）。 */
static void _p25rls_cmd_fill(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb;

    if (payload_len < 1U) {
        printf("[p25rls] FS 载荷为空，整帧丢弃\n");
        return;
    }

    dev_display_color_t color;
    if (!_p25rls_map_color(*(const uint8_t *)payload, &color)) {
        printf("[p25rls] FS 颜色码 %u 非 1/2/3，整帧丢弃\n", (unsigned)*(const uint8_t *)payload);
        return;
    }

    _p25rls_fill_screen(color, false);
}

/** @brief `HZ` 文字显示（全屏）：颜色 + 点阵大小 + GBK 文本
 *
 *  协议没有坐标/字型字段 → 固定宋体、从左上角起排、区域为整个逻辑屏。
 *  **整屏替换**：先清整屏再画，否则换一条更短的字会新旧叠在一起。
 *  超宽按字形截断（`word_wrap = false`），显式 `\n` 逐行下移一个字号。
 *  落盘：文档只承诺 SV 断电保存，故文字**不落盘**（上电恢复的是最后一次 SV 的图）。
 *  @param ccb 本帧来源通道（协议未定义应答，不使用） */
static void _p25rls_cmd_text(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb;

    if (payload_len <= sizeof(app_p25rls_text_t)) {
        printf("[p25rls] HZ 载荷 %u B 不足（固定头 %u B + 至少 1 B 文本），整帧丢弃\n",
               (unsigned)payload_len, (unsigned)sizeof(app_p25rls_text_t));
        return;
    }

    const app_p25rls_text_t *ctx = (const app_p25rls_text_t *)payload;
    const uint16_t text_len      = (uint16_t)(payload_len - sizeof(app_p25rls_text_t));

    dev_display_color_t color;
    if (!_p25rls_map_color(ctx->color, &color)) {
        printf("[p25rls] HZ 颜色码 %u 非 1/2/3，整帧丢弃\n", (unsigned)ctx->color);
        return;
    }

    const app_font_size_t size = _p25rls_map_dot_size(ctx->dot_size);

    _p25rls_fill_screen(DEV_DISPLAY_COLOR_BLACK, false);

    app_render(&(app_render_cfg_t){
        .type   = APP_RENDER_TYPE_TEXT,
        .x      = 0,
        .y      = 0,
        .w      = app_screen_rows(),
        .h      = app_screen_cols(),
        .style  = &(app_render_style_t){
            .h_align = APP_RENDER_ALIGN_LEFT_UP,
            .v_align = APP_RENDER_ALIGN_LEFT_UP,
        },
        .color     = color,
        .font_size = size,
        .font_type = APP_FONT_TYPE_ST, /* 协议无字型字段，固定宋体 */
        .text_enc  = APP_FONT_ENC_GBK,
        .text      = (const char *)ctx->text,
        .len       = text_len,
    });
}

/** @brief `CL` 清屏：整屏填黑，不落盘 */
static void _p25rls_cmd_clear(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb;
    (void)payload;
    (void)payload_len;

    _p25rls_fill_screen(DEV_DISPLAY_COLOR_BLACK, false);
}

/** @brief `BD` 修改波特率：**只接受、不动作**
 *
 *  本产品走 UDP，这条命令没有作用对象；串口那条路（RS232-1）不在本协议里。
 *  真要做波特率切换，`pl_uart_set_baud()` 已经就绪（GZZH 用它降到 9600）。
 *  协议未定义应答，不回包。 */
static void _p25rls_cmd_baud(app_ccb_t *ccb, const void *payload, uint16_t payload_len)
{
    (void)ccb;

    if (payload_len < 1U) return;

    printf("[p25rls] BD 波特率码 0x%02X：本协议走 UDP，忽略（仅记录）\n",
           (unsigned)*(const uint8_t *)payload);
}
