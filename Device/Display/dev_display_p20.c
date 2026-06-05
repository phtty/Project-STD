/**
 * @file    dev_display_p20.c
 * @brief   P20 模组派生类型 — 静态扫描、2 通道、R/G/B 分离
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

#define GROUP_SIZE 16

/* ---- P20 参数 ---- */
#define P20_MODULE_ROWS         1
#define P20_MODULE_COLS         1
#define P20_MODULE_PIXEL_ROW    16
#define P20_MODULE_PIXEL_COL    8
#define P20_CHANNELS_PER_MODULE 2
#define P20_SCAN_LINES          1

/* ---- 派生参数 ---- */
#define P20_SCREEN_ROWS    (P20_MODULE_ROWS * P20_MODULE_PIXEL_ROW)
#define P20_SCREEN_COLS    (P20_MODULE_COLS * P20_MODULE_PIXEL_COL)
#define P20_BUFFER_SIZE    (P20_SCREEN_ROWS * P20_SCREEN_COLS)
#define P20_TOTAL_CHANNELS (P20_MODULE_COLS * P20_CHANNELS_PER_MODULE)
#define P20_CHANNEL_PIXELS (P20_MODULE_PIXEL_ROW * P20_MODULE_PIXEL_COL * P20_MODULE_ROWS / P20_TOTAL_CHANNELS)
#define P20_SCAN_LINE_PX   (P20_CHANNEL_PIXELS / P20_SCAN_LINES)

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} p20_bsrr_t;

[[gnu::section(".ccmram")]] static p20_bsrr_t g_bsrr[P20_TOTAL_CHANNELS][8];

/* ---- P20 实例 ---- */
typedef struct {
    dev_display_t dev;
} dev_display_p20_t;

[[gnu::section(".ccmram")]] static uint8_t p20_pixel_map[P20_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t p20_hub75_buff[P20_BUFFER_SIZE];

static dev_display_p20_t g_p20 = {
    .dev = {
        .ops                 = nullptr, /* 由 _p20_init 设置 */
        .module_rows         = P20_MODULE_PIXEL_ROW,
        .module_cols         = P20_MODULE_PIXEL_COL,
        .channels_per_module = P20_CHANNELS_PER_MODULE,
        .modules_per_row     = P20_MODULE_ROWS,
        .modules_per_col     = P20_MODULE_COLS,
        .screen_rows         = P20_SCREEN_ROWS,
        .screen_cols         = P20_SCREEN_COLS,
        .total_channels      = P20_TOTAL_CHANNELS,
        .channel_pixels      = P20_CHANNEL_PIXELS,
        .scan_line_pixels    = P20_SCAN_LINE_PX,
        .buffer_size         = P20_BUFFER_SIZE,
        .pixel_map           = p20_pixel_map,
        .hub75_buff          = p20_hub75_buff,
        .light_level         = 7,
    },
};

dev_display_t *dev_display_p20_get(void)
{
    return &g_p20.dev;
}

/* ---- prepare: pixel_map → hub75_buff ---- */
static void _p20_prepare(dev_display_t *dev)
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
static void _p20_scan(dev_display_t *dev, uint8_t line)
{
    for (uint16_t l = 0; l < dev->scan_line_pixels; l++) {
        uint16_t base = (uint16_t)line * dev->scan_line_pixels + l;
        for (uint8_t ch = 0; ch < dev->total_channels; ch++) {
            p20_bsrr_t *b = &g_bsrr[ch][dev->hub75_buff[base + ch * dev->channel_pixels]];
            pl_hub75_bsrr_flush(&b->r);
            pl_hub75_bsrr_flush(&b->g);
            pl_hub75_bsrr_flush(&b->b);
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: ABCD 编码 (1/4 扫用 A,B 两线) ---- */
static void _p20_set_row(uint8_t row)
{
    pl_hub75_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t p20_ops = {
    .prepare    = _p20_prepare,
    .scan       = _p20_scan,
    .set_row    = _p20_set_row,
    .scan_lines = P20_SCAN_LINES,
};

/* ---- 自动初始化：填充 BSRR + 绑定 ops ---- */
void dev_display_p20_init(void)
{
    g_p20.dev.ops = &p20_ops;

    for (uint8_t ch = 0; ch < g_p20.dev.total_channels; ch++) {
        for (uint8_t c = 0; c < 8; c++) {
            g_bsrr[ch][c].r.port = g_hub75_pin_r[ch].port;
            g_bsrr[ch][c].r.val  = (c & 1) ? (uint32_t)g_hub75_pin_r[ch].pin
                                           : (uint32_t)g_hub75_pin_r[ch].pin << 16;
            g_bsrr[ch][c].g.port = g_hub75_pin_g[ch].port;
            g_bsrr[ch][c].g.val  = (c & 2) ? (uint32_t)g_hub75_pin_g[ch].pin
                                           : (uint32_t)g_hub75_pin_g[ch].pin << 16;
            g_bsrr[ch][c].b.port = g_hub75_pin_b[ch].port;
            g_bsrr[ch][c].b.val  = (c & 4) ? (uint32_t)g_hub75_pin_b[ch].pin
                                           : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }
}
hw_dev_initcall(dev_display_p20_init);
