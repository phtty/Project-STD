/**
 * @file    app_render.h
 * @brief   文字/图形渲染 API（依赖注入 dev_display + dev_flash_font）
 */

#pragma once

#include <stdint.h>
#include "dev_display.h"
#include "dev_flash_font.h"

typedef enum {
    FONT_SIZE_16 = 16,
    FONT_SIZE_24 = 24,
    FONT_SIZE_32 = 32,
} font_size_t;

typedef enum {
    ALIGN_LEFT   = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT  = 2,
} text_align_t;

void app_render_init(display_dev_t *display, font_flash_dev_t *font);

void app_render_string(display_dev_t *display, font_flash_dev_t *font,
                       uint16_t x, uint16_t y,
                       const char *text, uint16_t len,
                       hub75_color_t color, font_size_t size);

void app_render_fill(display_dev_t *display, hub75_color_t color);

uint16_t app_render_auto_x(uint16_t len, font_size_t size, text_align_t align);

uint16_t app_render_auto_y(uint8_t line, font_size_t size);
