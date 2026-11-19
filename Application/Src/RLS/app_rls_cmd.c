/**
 * @file    app_rls_cmd.c
 * @brief   RLS 命令处理实现：测试帧与图文显示
 */

#include "app_rls_cmd.h"
#include "app_screen.h"

#include <stdio.h>

#include "app_render.h"
#include "app_light_sensor.h"
#include "app_iap_cfg.h"
#include "app_tcp_server.h"
#include "pl_net.h"
#include "bcc_utils.h"

static void _rls_cmd_test(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_display_save(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_display_sw(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_aquire_ip(app_ccb_t *ccb, void *data, uint16_t data_len);

/* 顺序必须与 app_rls.c 的 cmd_index_table 逐项一致；搜索命令两种线上码共用同一处理 */
const app_rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    _rls_cmd_test,
    _rls_cmd_display,
    _rls_cmd_display_save,
    _rls_cmd_display_sw,     /* 线上 "55"（0x35 0x35） */
    _rls_cmd_display_sw,     /* 线上 0x35 0x55（参考分支的读法） */
    _rls_cmd_set_ip,
    _rls_cmd_aquire_ip, /* 线上 "ST"（文档数值列） */
    _rls_cmd_aquire_ip, /* 线上 "SC"（文档描述列） */
};

bool app_rls_cmd_should_echo(app_rls_cmd_type_t cmd)
{
    switch (cmd) {
        /* 发图并显示的两条：不回显（见头文件） */
        case APP_RLS_CMD_TYPE_DISPLAY:
        case APP_RLS_CMD_TYPE_DISPLAY_SAVE: return false;

        /* 其余已实现的命令都回显 */
        case APP_RLS_CMD_TYPE_TEST:
        case APP_RLS_CMD_TYPE_DISPLAY_SW:
        case APP_RLS_CMD_TYPE_DISPLAY_SW_ALT:
        case APP_RLS_CMD_TYPE_SET_IP:
        case APP_RLS_CMD_TYPE_AQUIRY_IP:
        case APP_RLS_CMD_TYPE_AQUIRY_IP_SC: return true;

        default: return false; /* 未识别的命令码不播回去 */
    }
}

[[maybe_unused]] static void _rls_cmd_test(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

/** @brief 整屏 1bpp 位图所需字节 = ceil(宽/8) × 高
 *
 *  纯函数、可测；口径与 `app_render` 的 BITMAP 源（每行 (w+7)/8 字节、行优先、
 *  MSB first）以及 `app_render_persist_t` 的位图布局一致。
 *
 *  本工程两块板的实际数字：
 *   · 3833024 单卡逻辑屏 128×32 → (128/8)×32 = 16×32 = **512 字节**，
 *     正好等于 `RLS_PAYLOAD_MAX` 为位图预留的那点空间，一帧装得下；
 *   · 5006048 级联逻辑屏 224×100 → (224/8)×100 = 28×100 = **2800 字节**，
 *     而 RLS 一帧最多带 ~521 字节（530 − 帧头 6 − BCC 1 − 尾 2，再减显示头 4）
 *     —— **5006048 上必然拒显**（帧里根本放不下整屏位图）。 */
static uint16_t _rls_display_bitmap_bytes(uint16_t w, uint16_t h)
{
    return (uint16_t)((((uint32_t)w + 7U) / 8U) * h);
}

/** @brief 显示帧里实际能带多少位图字节：整帧 − 帧头(6) − 显示头(4) − BCC(1) − 尾(2)
 *
 *  整帧 `data_len` < 13 时无位图可言（无符号减法会回绕，先判）。 */
static uint16_t _rls_display_bitmap_have(uint16_t data_len)
{
    const uint16_t overhead = (uint16_t)(sizeof(app_rls_frame_t) + sizeof(app_rls_display_t) + 3U);
    return (data_len > overhead) ? (uint16_t)(data_len - overhead) : 0U;
}

/** @brief 显示前校验帧内位图字节是否够铺满逻辑屏；不足则**拒显**并打日志
 *
 *  口径取 **≥**：多带的字节不用、也不拒；不足则连 `app_render` 都不调
 *  （否则 `app_render` 会按逻辑屏几何越界读帧缓冲）。 */
static bool _rls_display_bitmap_ok(uint16_t data_len)
{
    const uint16_t rows = app_screen_rows();
    const uint16_t cols = app_screen_cols();
    const uint16_t need = _rls_display_bitmap_bytes(rows, cols);
    const uint16_t have = _rls_display_bitmap_have(data_len);

    if (have >= need) return true;

    printf("[rls] 显示帧位图不足：帧内 %u B < 逻辑屏 %ux%u 所需 %u B，拒显\n", (unsigned)have,
           (unsigned)rows, (unsigned)cols, (unsigned)need);
    return false;
}

/** @brief 线上颜色字节 → 面板颜色（协议：0 红 / 1 绿 / 2 黄）；表外返回 false
 *
 *  **必须映射、不能直传**，两层原因：
 *   1) `dev_display_color_t` 的下标是 BLACK=0 / RED=1 / GREEN=2 / YELLOW=3 ——
 *      直传会把"红"画成灭、"绿"画成红、"黄"画成绿；
 *   2) 越界值会一路落到板级扫描驱动的 `s_bsrr_table[ch][color]`（`_scan` 里按
 *      帧缓冲里的字节查表）。越界读出来的 `port` 是 .ccmram 里的垃圾指针，
 *      扫描中断里 `p->port->BSRR = p->val` 就是**往任意地址写** —— 不是显示错色，
 *      是乱写内存。所以这里是**闭合映射**：表外值整帧拒显。
 *
 *  口径取文档里出现 3 次的 `0/1/2`（BM / DS / PIC 三张表的"数值"列）。SV 那张的
 *  "数值"列写成 `1/2/3`，与它自己的描述栏（0红/1绿/2黄）矛盾；若现场抓包发现
 *  上位机按 SV 那张发，只改这张表 —— 不要改成"两者都收"，1/2 在两张表里含义不同。 */
static bool _rls_map_color(uint8_t v, dev_display_color_t *out)
{
    static const dev_display_color_t map[] = {
        DEV_DISPLAY_COLOR_RED,    /* 0 = 红 */
        DEV_DISPLAY_COLOR_GREEN,  /* 1 = 绿 */
        DEV_DISPLAY_COLOR_YELLOW, /* 2 = 黄 */
    };
    if (v >= sizeof(map) / sizeof(map[0])) return false;

    *out = map[v];
    return true;
}

/** @brief 应用显示帧的亮度字节（协议：0~31，0 最暗 / 31 最亮；255 = 自动）
 *
 *  **口径**：文档三张显示命令表都写"亮度（32 级），0~31，255 自动亮度"。
 *  面板只有 3 位亮度（0~7，0 = 全灭），所以 0~31 线性压到 **1~7**：0 落 1 而不是 0，
 *  因为 0 在硬件上是"不亮"，而文档里 0 是"最暗"、关屏另有 DISPLAY_SW 命令。
 *  32~254 是协议外值，按最亮（7）处理 —— 它们不该让"显示"这件事失败。
 *
 *  与参考分支（rls_p20）的差别：那一版把 0 当关屏、把 DISPLAY_SW 的 0x30/0x31
 *  也当亮度值喂进同一个函数（结果 0x30 也落到"最亮 7"，关屏关不掉）。这里亮度只
 *  来自显示命令的亮度字段，开关留给 DISPLAY_SW 自己解释，别复用本函数。
 */
static void _rls_apply_light_level(uint8_t level)
{
    if (level == 255U) {
        app_light_sensor_resume(); /* 恢复光感跟随并立即调光一次 */
        return;
    }
    if (level > 31U) level = 31U;

    /* 0~31 → 1~7（四舍五入）：31 → 1 + (186+15)/31 = 7，0 → 1 */
    const uint8_t hw = (uint8_t)(1U + (uint16_t)((uint16_t)level * 6U + 15U) / 31U);
    app_light_sensor_set_fixed(hw);
}

static void _rls_cmd_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb; /* 回复走 app_ccb_send_to 的表来源，不需要本帧的通道 */
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

    if (!_rls_display_bitmap_ok(data_len)) return; // 位图不足：拒显，不碰 app_render

    dev_display_color_t color;
    if (!_rls_map_color(display_ctx->color, &color)) {
        printf("[rls] 显示帧颜色 %u 不在协议约定（0红/1绿/2黄）内，拒显\n",
               (unsigned)display_ctx->color);
        return;
    }

    _rls_apply_light_level(display_ctx->light_level); /* 显示命令同步应用亮度 */

    // 显示前先清屏
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    app_render(&(app_render_cfg_t){
        .type   = APP_RENDER_TYPE_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = app_screen_rows(),
        .h      = app_screen_cols(),
        .color  = color,
        .bitmap = display_ctx->bitmap,
    });
}

static void _rls_cmd_display_save(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

    if (!_rls_display_bitmap_ok(data_len)) return; // 位图不足：拒显，不碰 app_render

    dev_display_color_t color;
    if (!_rls_map_color(display_ctx->color, &color)) {
        printf("[rls] 显示帧颜色 %u 不在协议约定（0红/1绿/2黄）内，拒显\n",
               (unsigned)display_ctx->color);
        return;
    }

    _rls_apply_light_level(display_ctx->light_level); /* 显示命令同步应用亮度 */

    // 显示前先清屏
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    app_render(&(app_render_cfg_t){
        .type    = APP_RENDER_TYPE_BITMAP,
        .x       = 0,
        .y       = 0,
        .w       = app_screen_rows(),
        .h       = app_screen_cols(),
        .color   = color,
        .bitmap  = display_ctx->bitmap,
        /* **内容定稿之后才存**（app_screen 在落屏时取走这个请求）。
           原来是这里紧跟着调 app_render_save() —— 那时画布还没落屏，
           存下去的是上一帧。 */
        .persist = true,
    });
}

/** @brief 显示开/关：DATA 单字节 0x31 = 打开显示、0x30 = 关闭显示
 *
 *  只管**面板输出使能**（`app_screen_oe_*`），不动亮度 —— 开关与亮度是两个状态，
 *  这是 CDSTD A0H / A2H 定下的口径（用"设亮度 0"实现关屏会把关屏前的档位永久丢掉）。
 *  因此关屏再开屏能回到原档位，光感跟随也不会被打断。
 *
 *  **不能拿这个字节当亮度喂给 `_rls_apply_light_level()`** —— 参考分支就是这么做的，
 *  0x31/0x30 双双落进 default → 都设成"最亮 7"，"关闭显示"关不掉屏。 */
static void _rls_cmd_display_sw(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb; /* 协议未定义应答帧 */
    const uint16_t need = (uint16_t)(sizeof(app_rls_frame_t) + 1U + 3U); /* 6 + 1 + BCC1 + 尾2 */

    if (data_len < need) {
        printf("[rls] 开关显示帧长度 %u < %u，丢弃\n", (unsigned)data_len, (unsigned)need);
        return;
    }

    const uint8_t sw = *(const uint8_t *)data;
    switch (sw) {
        case 0x31: app_screen_oe_enable(); break;  /* '1' 打开显示 */
        case 0x30: app_screen_oe_disable(); break; /* '0' 关闭显示 */
        default: printf("[rls] 开关显示参数 0x%02X 非 0x30/0x31，忽略\n", (unsigned)sw); break;
    }
}

/* ================================================================
 *  网络类命令：修改 IP（IP）/ 搜索设备（ST、SC）
 * ================================================================ */

/** @brief 修改 IP：DATA = IP[4] | 掩码[4] | 网关[4]（整帧 21 字节）
 *
 *  **只写 IAP 记录、不在运行态生效** —— 与 IAP 0AH / LDI 06H 同口径：运行中换 IP 会把
 *  当前连接连同这条命令的后果一起断掉，新地址下次上电生效。
 *
 *  端口沿用设备当前配置：记录有效就取记录里的（那条记录本来就是运行态端口的镜像），
 *  没有记录区（直烧板）才回落到运行态 TCP 端口。**不能固定传 0** —— 记录里的 port
 *  会被覆盖成 0，Recovery/上位机读到的端口就废了。
 *
 *  帧长不足直接丢弃（不产生任何副作用）：12 字节载荷以外还可能带别的字节，所以口径是
 *  **≥** 而不是 ==。 */
static void _rls_cmd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb; /* 协议未定义应答帧 */
    const uint16_t need = (uint16_t)(sizeof(app_rls_frame_t) + sizeof(app_rls_net_cfg_t) + 3U);

    if (data_len < need) {
        printf("[rls] 修改 IP 帧长度 %u < %u，丢弃\n", (unsigned)data_len, (unsigned)need);
        return;
    }

    const app_rls_net_cfg_t *cfg = (const app_rls_net_cfg_t *)data;

    uint32_t port = app_tcp_server_get_port();
    app_flash_iap_net_cfg_t rec;
    if (app_iap_get_net_cfg(&rec)) port = rec.port;

    app_flash_iap_update_net_cfg(cfg->ip, cfg->mask, cfg->gw, port);

    printf("[rls] 设置 IP %u.%u.%u.%u / %u.%u.%u.%u / %u.%u.%u.%u：已写入 IAP 记录，重启后生效\n",
           (unsigned)cfg->ip[0], (unsigned)cfg->ip[1], (unsigned)cfg->ip[2], (unsigned)cfg->ip[3],
           (unsigned)cfg->mask[0], (unsigned)cfg->mask[1], (unsigned)cfg->mask[2],
           (unsigned)cfg->mask[3], (unsigned)cfg->gw[0], (unsigned)cfg->gw[1],
           (unsigned)cfg->gw[2], (unsigned)cfg->gw[3]);
}

/** @brief 搜索设备：回一帧 REPORT_IP（"RB"），携带本机当前生效的 IP/掩码/网关
 *
 *  命令里的地址字段是上位机自己带的，**不参与应答内容** —— 报文语义是"谁在线、报出
 *  你的地址"，回运行态地址（`pl_net_get_ip`）而不是 IAP 记录里的旧值。
 *
 *  BCC 口径按文档正文："信息帧从帧头到 BCC 字节之前所有字节的异或" → 覆盖前 18 字节
 *  （帧头 2 + 长度 2 + 命令 2 + 地址 12）。参考分支（rls_p20）只对 12 字节数据域做异或，
 *  与正文不符；若现场上位机校验不过，把这里的长度改成 12 试一次即可定位。 */
static void _rls_cmd_aquire_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;

    uint8_t frame[RLS_REPORT_IP_LEN];
    uint8_t ip[4], mask[4], gw[4];

    pl_net_get_ip(ip, mask, gw);

    frame[0] = 0xFF;
    frame[1] = 0xFE;
    frame[2] = 0x00;
    frame[3] = (uint8_t)RLS_REPORT_IP_LEN; /* 21：高字节恒 0，见文档"长度高字节在前" */
    /* 命令码按小端写，与接收侧 `*(app_rls_cmd_type_t *)frame->cmd` 的读法一致 */
    frame[4] = (uint8_t)(APP_RLS_CMD_TYPE_REPORT_IP & 0xFFU);
    frame[5] = (uint8_t)((APP_RLS_CMD_TYPE_REPORT_IP >> 8) & 0xFFU);
    memcpy(frame + 6, ip, 4);
    memcpy(frame + 10, mask, 4);
    memcpy(frame + 14, gw, 4);
    frame[18] = bcc_calc(frame, 18U); /* 帧头 → 数据域末尾，不含 BCC 自身 */
    frame[19] = 0x0D;
    frame[20] = 0x0C;

    if (app_ccb_send(ccb, frame, (uint16_t)sizeof(frame)) < 0)
        printf("[rls] 搜索应答发送失败（通道不可用）\n");
}
