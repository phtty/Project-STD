#include "app_rls_cmd.h"
#include "app_screen.h"

#include "app_render.h"

static void cmd_test(ccb_t *ccb, void *data);
static void cmd_display(ccb_t *ccb, void *data);
static void cmd_display_save(ccb_t *ccb, void *data);

const rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    cmd_test,
    cmd_display,
    cmd_display_save,
};

[[maybe_unused]] static void cmd_test(ccb_t *ccb, void *data)
{
    (void)ccb;
    (void)data;
}

static void cmd_display(ccb_t *ccb, void *data)
{
    (void)ccb; /* 回复走 ccb_send_to 的表来源，不需要本帧的通道 */
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = app_screen_rows(),
        .h      = app_screen_cols(),
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
}

static void cmd_display_save(ccb_t *ccb, void *data)
{
    (void)ccb;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render(&(render_cfg_t){
        .type    = RENDER_BITMAP,
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
