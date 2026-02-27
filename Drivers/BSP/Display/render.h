#ifndef DRIVERS_BSP_DISPLAY_RENDER_H
#define DRIVERS_BSP_DISPLAY_RENDER_H

#include "main.h"
#include "display.h"
#include "stdbool.h"

// 步进值
#define ASCII_16_STEP (1536U)
#define GBK_16_STEP   (766080U)
#define ASCII_24_STEP (4608U)
#define GBK_24_STEP   (1723680U)
#define ASCII_32_STEP (6144U)
#define GBK_32_STEP   (3064320U)

// 初始偏移值
#define ASCII_16_OFFSET (0U)
#define GBK_16_OFFSET   (ASCII_16_OFFSET + 4 * ASCII_16_STEP)
#define ASCII_24_OFFSET (GBK_16_OFFSET + 4 * GBK_16_STEP)
#define GBK_24_OFFSET   (ASCII_24_OFFSET + 4 * ASCII_24_STEP)
#define ASCII_32_OFFSET (GBK_24_OFFSET + 4 * GBK_24_STEP)
#define GBK_32_OFFSET   (ASCII_32_OFFSET + 4 * ASCII_32_STEP)

// 字号定义
typedef enum {
    font_16,
    font_24,
    font_32,
    font_48,
} FontSize_t;

// 字高定义
typedef enum {
    font16 = 16,
    font24 = 24,
    font32 = 32,
    font48 = 48,
} FontHigh_t;

// 字型定义
typedef enum {
    font_st,
    font_fs,
    font_kt,
    font_ht,
} FontType_t;

extern uint16_t max_display_len;

void Disp_Fill(DispColor_t color);

void RenderString(uint32_t start_x, uint32_t start_y, const uint8_t *p_text, uint32_t text_len, DispColor_t color, FontSize_t font_size, FontType_t font_type);

void RenderChar(const uint8_t *p_text, uint16_t *x, uint16_t *y, FontSize_t font_size, FontType_t font_type, bool mode, bool line_break, DispColor_t color);

uint8_t auto_font_size(uint16_t len, uint8_t size);
uint16_t auto_line(uint8_t line, uint8_t font_size);
uint16_t set_align(uint16_t len, uint8_t font_size, uint8_t align);

#endif // DRIVERS_BSP_DISPLAY_RENDER_H
