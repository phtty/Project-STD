#ifndef DRIVERS_BSP_DISPLAY_RENDER_H
#define DRIVERS_BSP_DISPLAY_RENDER_H

#include "main.h"
#include "time.h"
#include "stdbool.h"

// 屏幕数据
#define SCREEN_PIXEL_ROW (120U)
#define SCREEN_PIXEL_COL (240U)
#define DISRAM_SIZE      (SCREEN_PIXEL_ROW * SCREEN_PIXEL_COL)

// 步进值(单个字模的字节数)
#define ASCII_14_STEP (1344U)
#define GBK_14_STEP   (670320U)
#define ASCII_16_STEP (1536U)
#define GBK_16_STEP   (766080U)
#define ASCII_20_STEP (3840U)
#define GBK_20_STEP   (1436400U)
#define ASCII_24_STEP (4608U)
#define GBK_24_STEP   (1723680U)
#define ASCII_32_STEP (6144U)
#define GBK_32_STEP   (3064320U)

// 初始偏移值
#define ASCII_14_OFFSET (0U)
#define GBK_14_OFFSET   (ASCII_14_OFFSET + 4 * ASCII_14_STEP)
#define ASCII_16_OFFSET (GBK_14_OFFSET + 4 * GBK_14_STEP)
#define GBK_16_OFFSET   (ASCII_16_OFFSET + 4 * ASCII_16_STEP)
#define ASCII_20_OFFSET (GBK_16_OFFSET + 4 * GBK_16_STEP)
#define GBK_20_OFFSET   (ASCII_20_OFFSET + 4 * ASCII_20_STEP)
#define ASCII_24_OFFSET (GBK_20_OFFSET + 4 * GBK_20_STEP)
#define GBK_24_OFFSET   (ASCII_24_OFFSET + 4 * ASCII_24_STEP)
#define ASCII_32_OFFSET (GBK_24_OFFSET + 4 * GBK_24_STEP)
#define GBK_32_OFFSET   (ASCII_32_OFFSET + 4 * ASCII_32_STEP)

// 字号定义
typedef enum {
    font_14,
    font_16,
    font_20,
    font_24,
    font_32,
} FontSize_t;

// 字高定义
typedef enum {
    font14 = 14,
    font16 = 16,
    font20 = 20,
    font24 = 24,
    font32 = 32,
} FontHigh_t;

// 字型定义
typedef enum {
    font_st,
    font_fs,
    font_kt,
    font_ht,
} FontType_t;

// 颜色定义
typedef enum {
    black,
    red,
    green,
    yellow,
    white,
} DispColor_t;

extern uint16_t max_display_len;

void Disp_Fill(DispColor_t color, uint32_t start_y);

void RenderString(uint32_t start_x, uint32_t start_y, const uint8_t *p_text, uint32_t text_len, DispColor_t color, FontSize_t font_size, FontType_t font_type, bool line_break);

void RenderChar(const uint8_t *p_text, uint16_t *x, uint16_t *y, FontSize_t font_size, FontType_t font_type, bool mode, bool line_break, DispColor_t color);

uint8_t auto_font_size(uint16_t len, uint8_t size);
uint16_t auto_line(uint8_t line, uint8_t font_size);
uint16_t set_align(uint16_t len, uint8_t font_size, uint8_t align);

#endif // DRIVERS_BSP_DISPLAY_RENDER_H
