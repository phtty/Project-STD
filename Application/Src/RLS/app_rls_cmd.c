/**
 * @file    app_rls_cmd.c
 * @brief   RLS 命令处理实现：测试帧与图文显示
 */

#include "app_rls_cmd.h"
#include "app_screen.h"

#include <stdio.h>

#include "app_render.h"

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

static void _rls_cmd_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb; /* 回复走 app_ccb_send_to 的表来源，不需要本帧的通道 */
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

    if (!_rls_display_bitmap_ok(data_len)) return; // 位图不足：拒显，不碰 app_render

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
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
}

static void _rls_cmd_display_save(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

    if (!_rls_display_bitmap_ok(data_len)) return; // 位图不足：拒显，不碰 app_render

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
        .color   = display_ctx->color,
        .bitmap  = display_ctx->bitmap,
        /* **内容定稿之后才存**（app_screen 在落屏时取走这个请求）。
           原来是这里紧跟着调 app_render_save() —— 那时画布还没落屏，
           存下去的是上一帧。 */
        .persist = true,
    });
}
