#include "app_vms_ctrl.h"

#include "string.h"

#include "app_render.h"

static void vms_display_ctrl(ldi_ctrl_vms_t ctx)
{
    render_cfg_t txt_cfg = {
        .type     = RENDER_TEXT,
        .x        = 0,
        .y        = 0,
        .w        = dev_display_get()->screen_rows,
        .h        = dev_display_get()->screen_cols,
        .text_enc = FONT_ENC_GBK,
    };

    app_render(&txt_cfg);
}

static void vms_clean_ctrl(ldi_ctrl_vms_t ctx)
{
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_WHITE,
    });
}

void vms_ctrl(ldi_ctrl_vms_t ctx)
{
    if (ctx.device_func_type == 0x01)
        vms_display_ctrl(ctx);
    else if (ctx.device_func_type == 0x02)
        vms_clean_ctrl(ctx);
}
