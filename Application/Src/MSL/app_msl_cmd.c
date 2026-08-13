#include "app_msl_cmd.h"

#include "app_render.h"
#include "app_light_sensor.h"

static void cmd_test(channel_t *ch, void *data);
static void cmd_text(channel_t *ch, void *data);
static void cmd_bitmap(channel_t *ch, void *data);
static void cmd_fill(channel_t *ch, void *data);
static void cmd_lightlevel(channel_t *ch, void *data);

const msl_cmd_handler_fn_t g_msl_cmd_table[] = {
    cmd_test,
    cmd_text,
    cmd_bitmap,
    cmd_fill,
    cmd_lightlevel,
};

[[maybe_unused]] static void cmd_test(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
}

/**
 * @brief 同步显示，文字同步
 */
static void cmd_text(channel_t *ch, void *data)
{
    msl_text_t *ctx = data;

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = (ctx->x[1] & 0xFF) | ((ctx->x[0] << 8) & 0xFF00),
        .y     = (ctx->y[1] & 0xFF) | ((ctx->y[0] << 8) & 0xFF00),
        .w     = dev_display_get()->screen_rows,
        .h     = dev_display_get()->screen_cols,
        .color = ctx->color,
        .style = &(render_style_t){
            .h_align   = ctx->col_style,
            .v_align   = ctx->row_style,
            .word_wrap = ctx->wrap,
        },
        .color     = ctx->color,
        .text      = (char *)(ctx->text),
        .len       = strlen((char *)(ctx->text)),
        .font_size = ctx->font_size,
        .font_type = ctx->font_type,
        .text_enc  = ctx->encode,
    });
    if (ctx->pers)
        app_render_save();
}

/**
 * @brief 同步显示，位图同步
 */
static void cmd_bitmap(channel_t *ch, void *data)
{
    msl_bitmap_t *ctx = data;

    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = ctx->width,
        .h      = ctx->high,
        .color  = ctx->color,
        .bitmap = ctx->bitmap,
    });
    if (ctx->pers)
        app_render_save();
}

/**
 * @brief 纯色填充
 */
static void cmd_fill(channel_t *ch, void *data)
{
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = *((display_color_t *)data),
    });
}

/**
 * @brief 手动指定亮度等级
 */
static void cmd_lightlevel(channel_t *ch, void *data)
{
    app_light_sensor_get()->display->light_level = *(uint8_t *)data;
}
