/**
 * @file    app_render.c
 * @brief   文字/图形渲染实现
 */

#include "app_render.h"

void app_render_init(dev_display_t *display, dev_storage_t *font)
{
    (void)display;
    (void)font;
    /* 初始化渲染上下文（后续从 render.c 迁移逻辑） */
}

void app_render_string(dev_display_t *display, dev_storage_t *font, uint16_t x, uint16_t y, const char *text, uint16_t len, hub75_color_t color, font_size_t size)
{
    (void)display;
    (void)font;
    (void)x;
    (void)y;
    (void)text;
    (void)len;
    (void)color;
    (void)size;
    /* 文字渲染（后续从 render.c 迁移逻辑） */
}

void app_render_fill(dev_display_t *display, hub75_color_t color)
{
    dev_display_fill(display, color);
}

uint16_t app_render_auto_x(uint16_t len, font_size_t size, text_align_t align)
{
    uint16_t width = len * (uint16_t)size / 2;
    switch (align) {
        case ALIGN_CENTER:
            return (dev_display_get()->screen_rows - width) / 2;
        case ALIGN_RIGHT:
            return dev_display_get()->screen_rows - width;
        default:
            return 0;
    }
}

uint16_t app_render_auto_y(uint8_t line, font_size_t size)
{
    return (uint16_t)line * (uint16_t)size;
}
