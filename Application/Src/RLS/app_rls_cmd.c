/**
 * @file    app_rls_cmd.c
 * @brief   RLS 命令处理实现：测试帧与图文显示
 */

#include "app_rls_cmd.h"
#include "app_screen.h"

#include <stdio.h>

#include "app_render.h"
#include "app_light_sensor.h"

static void _rls_cmd_test(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _rls_cmd_display_save(app_ccb_t *ccb, void *data, uint16_t data_len);

const app_rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    _rls_cmd_test,
    _rls_cmd_display,
    _rls_cmd_display_save,
};

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
