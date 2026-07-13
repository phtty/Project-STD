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
    rls_dispaly_t *frame  = (rls_dispaly_t *)(((rls_frame_t *)data)->data_bcc_tail);
    display_color_t color = COLOR_BLACK;

    switch (frame->color) {
        case 0x00:
            color = COLOR_RED;
            break;

        case 0x01:
            color = COLOR_GREEN;
            break;

        case 0x02:
            color = COLOR_YELLOW;
            break;

        default:
            break;
    }

    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = color,
        .bitmap = frame->bitmap,
    });
}

static void cmd_display_save(channel_t *ch, void *data)
{
}
