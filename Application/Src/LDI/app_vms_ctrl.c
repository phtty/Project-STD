#include "app_vms_ctrl.h"

#include "string.h"

#include "app_render.h"

static void vms_display_ctrl(ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    display_color_t color = COLOR_BLACK;
    if (ctx->font_color == 1) // 将ldi协议的颜色编号映射到文字渲染模块的颜色编号
        color = COLOR_GREEN;
    else if (ctx->font_color == 2)
        color = COLOR_RED;
    else if (ctx->font_color == 3)
        color = COLOR_YELLOW;

    render_style_t style = {
        .v_align   = ALIGN_CENTER,
        .word_wrap = false,
    };
    if (ctx->format == 1) // 将ldi协议的对齐方式映射到文字渲染模块的对齐方式
        style.h_align = ALIGN_CENTER;
    else if (ctx->format == 2)
        style.h_align = ALIGN_LEFT_UP;
    else if (ctx->format == 3)
        style.h_align = ALIGN_RIGHT_DOWN;

    font_size_t font_size = FONT_16;
    if (ctx->font_size == 0) // 将ldi协议的字号映射到文字渲染模块的字号
        font_size = FONT_SELF_ADAPT;
    else if (ctx->font_size == 1)
        font_size = FONT_16;
    else if (ctx->font_size == 2)
        font_size = FONT_24;
    else if (ctx->font_size == 3)
        font_size = FONT_32;

    for (uint16_t i = 0; i < text_len; i++) // LDI协议定义'_'为换行符
        if (ctx->text[i] == '_')
            ctx->text[i] = '\n';

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });

    // 配置文字渲染模块所需要的参数
    render_cfg_t txt_cfg = {
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = dev_display_get()->screen_rows,
        .h         = dev_display_get()->screen_cols,
        .color     = color,
        .text      = (char *)ctx->text,
        .len       = text_len,
        .style     = &style,
        .font_size = font_size,
        .font_type = FONT_HT,
        .text_enc  = FONT_ENC_GBK,
    };
    app_render(&txt_cfg);
    app_render_save(); // 显示持久化
}

static void vms_clean_ctrl(ldi_ctrl_vms_t *ctx)
{
    display_color_t color = COLOR_BLACK;
    if (ctx->clear_type == 0) // 将ldi协议的颜色编号映射到显示模块的颜色编号
        color = COLOR_BLACK;
    else if (ctx->clear_type == 1)
        color = COLOR_RED;
    else if (ctx->clear_type == 2)
        color = COLOR_GREEN;
    else if (ctx->clear_type == 3)
        color = COLOR_YELLOW;

    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = color,
    });
}

void vms_ctrl(ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    if (ctx->device_func_type == 0x01)
        vms_display_ctrl(ctx, text_len);
    else if (ctx->device_func_type == 0x02)
        vms_clean_ctrl(ctx);
}
