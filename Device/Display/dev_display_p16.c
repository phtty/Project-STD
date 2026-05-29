/**
 * @file    dev_display_p16.c
 * @brief   P16 模组派生类型 — 1/4 扫、2 通道、R/G/B 分离
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

#define GROUP_SIZE 16

/* ---- P16 参数 ---- */
#define P16_MODULE_ROWS        1
#define P16_MODULE_COLS        1
#define P16_MODULE_PIXEL_ROW   16
#define P16_MODULE_PIXEL_COL   8
#define P16_CHANNELS_PER_MODULE 2
#define P16_SCAN_LINES         4

/* ---- 派生参数 ---- */
#define P16_SCREEN_ROWS    (P16_MODULE_ROWS * P16_MODULE_PIXEL_ROW)
#define P16_SCREEN_COLS    (P16_MODULE_COLS * P16_MODULE_PIXEL_COL)
#define P16_BUFFER_SIZE    (P16_SCREEN_ROWS * P16_SCREEN_COLS)
#define P16_TOTAL_CHANNELS (P16_MODULE_COLS * P16_CHANNELS_PER_MODULE)
#define P16_CHANNEL_PIXELS (P16_MODULE_PIXEL_ROW * P16_MODULE_PIXEL_COL * P16_MODULE_ROWS / P16_TOTAL_CHANNELS)
#define P16_SCAN_LINE_PX   (P16_CHANNEL_PIXELS / P16_SCAN_LINES)

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    uint32_t r_val, g_val, b_val;
    GPIO_TypeDef *r_port, *g_port, *b_port;
} hub75_bsrr_t;

[[gnu::section(".ccmram")]] static hub75_bsrr_t g_bsrr[P16_TOTAL_CHANNELS][8];

/* ---- P16 实例 ---- */
typedef struct {
    dev_display_t dev;
} dev_display_p16_t;

[[gnu::section(".ccmram")]] static uint8_t p16_pixel_map[P16_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t p16_hub75_buff[P16_BUFFER_SIZE];

static dev_display_p16_t g_p16 = {
    .dev = {
        .ops                = NULL, /* 由 _p16_init 设置 */
        .module_rows        = P16_MODULE_PIXEL_ROW,
        .module_cols        = P16_MODULE_PIXEL_COL,
        .channels_per_module = P16_CHANNELS_PER_MODULE,
        .modules_per_row    = P16_MODULE_ROWS,
        .modules_per_col    = P16_MODULE_COLS,
        .screen_rows        = P16_SCREEN_ROWS,
        .screen_cols        = P16_SCREEN_COLS,
        .total_channels     = P16_TOTAL_CHANNELS,
        .channel_pixels     = P16_CHANNEL_PIXELS,
        .scan_line_pixels   = P16_SCAN_LINE_PX,
        .buffer_size        = P16_BUFFER_SIZE,
        .pixel_map          = p16_pixel_map,
        .hub75_buff         = p16_hub75_buff,
        .light_level        = 7,
    },
};

dev_display_t *dev_display_p16_get(void)
{
    return &g_p16.dev;
}

/* ---- prepare: pixel_map → hub75_buff ---- */
static void _p16_prepare(dev_display_t *dev)
{
    int32_t group_cnt = 0, row_cnt = 0, col_cnt = 0;

    for (int32_t map_cnt = 0; map_cnt < (int32_t)dev->buffer_size; map_cnt++) {
        row_cnt = map_cnt / (int32_t)dev->screen_rows;
        col_cnt = map_cnt % (int32_t)dev->screen_rows;

        group_cnt = col_cnt / 4 + (row_cnt / 4 * (dev->modules_per_row * 4));

        switch (row_cnt % 4) {
            case 0:
                dev->hub75_buff[2 * (3 - col_cnt % 4) + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 1:
                dev->hub75_buff[2 * (3 - col_cnt % 4) + 1 + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 2:
                dev->hub75_buff[2 * (col_cnt % 4) + 9 + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 3:
                dev->hub75_buff[2 * (col_cnt % 4) + 8 + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            default:
                break;
        }
    }
}

/* ---- scan: BSRR 查表 + CLK 脉冲 ---- */
static void _p16_scan(dev_display_t *dev, uint8_t line)
{
    for (uint16_t l = 0; l < dev->scan_line_pixels; l++) {
        uint16_t base = (uint16_t)line * dev->scan_line_pixels + l;
        for (uint8_t ch = 0; ch < dev->total_channels; ch++) {
            hub75_bsrr_t *b = &g_bsrr[ch][dev->hub75_buff[base + ch * dev->channel_pixels]];
            b->r_port->BSRR = b->r_val;
            b->g_port->BSRR = b->g_val;
            b->b_port->BSRR = b->b_val;
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: ABCD 编码 (1/4 扫用 A,B 两线) ---- */
static void _p16_set_row(uint8_t row)
{
    pl_hub75_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t p16_ops = {
    .prepare    = _p16_prepare,
    .scan       = _p16_scan,
    .set_row    = _p16_set_row,
    .scan_lines = P16_SCAN_LINES,
};

/* ---- 自动初始化：填充 BSRR + 绑定 ops ---- */
void dev_display_p16_init(void)
{
    g_p16.dev.ops = &p16_ops;

    for (uint8_t ch = 0; ch < g_p16.dev.total_channels; ch++) {
        for (uint8_t c = 0; c < 8; c++) {
            g_bsrr[ch][c].r_port = g_hub75_pin_r[ch].port;
            g_bsrr[ch][c].r_val  = (c & 1) ? (uint32_t)g_hub75_pin_r[ch].pin
                                           : (uint32_t)g_hub75_pin_r[ch].pin << 16;
            g_bsrr[ch][c].g_port = g_hub75_pin_g[ch].port;
            g_bsrr[ch][c].g_val  = (c & 2) ? (uint32_t)g_hub75_pin_g[ch].pin
                                           : (uint32_t)g_hub75_pin_g[ch].pin << 16;
            g_bsrr[ch][c].b_port = g_hub75_pin_b[ch].port;
            g_bsrr[ch][c].b_val  = (c & 4) ? (uint32_t)g_hub75_pin_b[ch].pin
                                           : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }
}
hw_dev_initcall(dev_display_p16_init);
