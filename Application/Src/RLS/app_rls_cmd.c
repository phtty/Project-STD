#include "app_rls_cmd.h"

#include "app_render.h"

static void cmd_test(channel_t *ch, void *data);
static void cmd_display(channel_t *ch, void *data);
static void cmd_display_save(channel_t *ch, void *data);

const rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    cmd_test,
    cmd_display,
    cmd_display_save,
};

[[maybe_unused]] static void cmd_test(channel_t *ch, void *data)
{
}

static void cmd_display(channel_t *ch, void *data)
{
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
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
}

static void cmd_display_save(channel_t *ch, void *data)
{
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
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
    app_render_save();
}
