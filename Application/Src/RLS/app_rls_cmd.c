#include "app_rls_cmd.h"
#include "app_screen.h"

#include "app_render.h"

static void _rls_cmd_test(app_ccb_t *ccb, void *data);
static void _rls_cmd_display(app_ccb_t *ccb, void *data);
static void _rls_cmd_display_save(app_ccb_t *ccb, void *data);

const app_rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    _rls_cmd_test,
    _rls_cmd_display,
    _rls_cmd_display_save,
};

[[maybe_unused]] static void _rls_cmd_test(app_ccb_t *ccb, void *data)
{
    (void)ccb;
    (void)data;
}

static void _rls_cmd_display(app_ccb_t *ccb, void *data)
{
    (void)ccb; /* 回复走 app_ccb_send_to 的表来源，不需要本帧的通道 */
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

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

static void _rls_cmd_display_save(app_ccb_t *ccb, void *data)
{
    (void)ccb;
    app_rls_display_t *display_ctx = (app_rls_display_t *)data;

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
