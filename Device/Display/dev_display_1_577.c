/**
 * @file    dev_display_1_577.c
 * @brief   1-577 模组派生类型 — P5-960x320 户外全彩 (3x3 模块)
 *
 * 模组参数：64x32 单模块，2 通道/模块，6 通道总计，ABC 1/8 扫描。
 * 实现 dev_display_ops: prepare (pixel_map->hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  1-577 模组参数 — P5-960x320 户外全彩 (3x3 模块)
 *
 *  单模块 64x32 像素，3 行 x 3 列共 9 模块，屏幕 192x96。
 *  HUB75 接口 2 通道/模块，3 列共 6 通道。
 *  ABC 3-bit 行地址，1/8 扫描。
 * ================================================================ */
#define MODULE_CODE                "1000000577"

#define _1_577_MODULE_ROWS         (1U)  /* 每行模块数 */
#define _1_577_MODULE_COLS         (1U)  /* 每列模块数 */
#define _1_577_MODULE_PIXEL_ROW    (64U) /* 单模块每行的像素个数 */
#define _1_577_MODULE_PIXEL_COL    (32U) /* 单模块每列的像素个数 */
#define _1_577_CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define _1_577_SCAN_LINES          (8U)  /* 1/8 扫描 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define _1_577_SCREEN_ROWS    (_1_577_MODULE_ROWS * _1_577_MODULE_PIXEL_ROW)
#define _1_577_SCREEN_COLS    (_1_577_MODULE_COLS * _1_577_MODULE_PIXEL_COL)
#define _1_577_BUFFER_SIZE    (_1_577_SCREEN_ROWS * _1_577_SCREEN_COLS)
#define _1_577_TOTAL_CHANNELS (_1_577_MODULE_COLS * _1_577_CHANNELS_PER_MODULE)
#define _1_577_CHANNEL_PIXELS (_1_577_MODULE_PIXEL_ROW * _1_577_MODULE_PIXEL_COL * _1_577_MODULE_ROWS / _1_577_CHANNELS_PER_MODULE)
#define _1_577_SCAN_LINE_PX   (_1_577_CHANNEL_PIXELS / _1_577_SCAN_LINES)

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} _1_577_bsrr_t;

[[gnu::section(".ccmram")]] static _1_577_bsrr_t g_bsrr[_1_577_TOTAL_CHANNELS][8];

/* ---- 1-577 实例 ---- */
typedef struct {
    dev_display_t me;
} dev_display_1_577_t;

[[gnu::section(".ccmram")]] static uint8_t _1_577_pixel_map[_1_577_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t _1_577_hub75_buff[_1_577_BUFFER_SIZE];

static dev_display_1_577_t g_1_577 = {
    .me = {
        .ops                 = nullptr,
        .module_rows         = _1_577_MODULE_PIXEL_ROW,
        .module_cols         = _1_577_MODULE_PIXEL_COL,
        .channels_per_module = _1_577_CHANNELS_PER_MODULE,
        .modules_per_row     = _1_577_MODULE_ROWS,
        .modules_per_col     = _1_577_MODULE_COLS,
        .scan_lines          = _1_577_SCAN_LINES,
        .screen_rows         = _1_577_SCREEN_ROWS,
        .screen_cols         = _1_577_SCREEN_COLS,
        .total_channels      = _1_577_TOTAL_CHANNELS,
        .channel_pixels      = _1_577_CHANNEL_PIXELS,
        .scan_line_pixels    = _1_577_SCAN_LINE_PX,
        .buffer_size         = _1_577_BUFFER_SIZE,
        .pixel_map           = _1_577_pixel_map,
        .hub75_buff          = _1_577_hub75_buff,
        .module_code         = MODULE_CODE,
        .light_level         = 7,
    },
};

dev_display_t *dev_display_1_577_get(void)
{
    return &g_1_577.me;
}

/* ================================================================
 *  prepare: pixel_map -> hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按 HUB75 物理移位顺序组织。
 *
 *  hub75_buff 布局（scan_line_pixels=384, channel_pixels=3072, 6ch）：
 *    ch0..ch5 各含 channel_pixels 字节，每通道内按 scan line 排列。
 *
 *  每条 scan line 的 384 字节内部排列（左32 + 间隔32 + 右32）× 3 列：
 *    [0..31]   = Y_high, X=0..31
 *    [32..63]  = Y_low,  X=0..31
 *    [64..95]  = Y_high, X=32..63
 *    [96..127] = Y_low,  X=32..63
 *    ...（后续列同理）
 *
 *  行映射（1/8 扫描，32 行模块）：
 *    ch0, line L → Y_low = L,    Y_high = L+8
 *    ch1, line L → Y_low = L+16, Y_high = L+24
 *    ch2, line L → Y_low = L+32, Y_high = L+40
 *    ch3, line L → Y_low = L+48, Y_high = L+56
 *    ch4, line L → Y_low = L+64, Y_high = L+72
 *    ch5, line L → Y_low = L+80, Y_high = L+88
 * ================================================================ */

static void _1_577_prepare(dev_display_t *dev)
{
    const uint8_t *pixel_map = dev->pixel_map;
    uint8_t *hub75_buff      = dev->hub75_buff;

    for (uint16_t y = 0; y < _1_577_SCREEN_COLS; y++) {
        const uint16_t ch       = y >> 4;                                /* 0-5 (6 channels) */
        const uint16_t local_y  = y & 0x0FU;                            /* 0-15 within channel */
        const uint16_t line     = local_y & 0x07U;                      /* scan line 0-7 */
        const uint16_t row_type = (local_y < 8U) ? 1U : 0U;            /* 1=low row, 0=high row */
        const uint16_t base     = ch * _1_577_CHANNEL_PIXELS
                                + line * _1_577_SCAN_LINE_PX
                                + row_type * 32U;

        const uint8_t *src = pixel_map + (uint16_t)(y * _1_577_SCREEN_ROWS);

        for (uint16_t x = 0; x < 32U; x++) {
            hub75_buff[base + x]       = src[x];             /* left half  X=0..31  */
            hub75_buff[base + 64U + x] = src[32U + x];       /* right half X=32..63 */
        }
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  6 通道，每像素输出 6 组 R/G/B。
 *  hub75_buff 布局：ch0..ch5 各含 channel_pixels(3072) 字节。
 *  每条 scan line 的384字节在 ch 内连续存放。
 * ================================================================ */

static inline void _1_577_scan(dev_display_t *dev, uint8_t line)
{
    const uint16_t scan_line_pixels = dev->scan_line_pixels;
    const uint16_t channel_pixels   = dev->channel_pixels;
    const uint8_t total_channels    = dev->total_channels;
    const _1_577_bsrr_t (*bsrr)[8]  = g_bsrr;
    const uint8_t *base             = dev->hub75_buff + (uint16_t)line * scan_line_pixels;

    for (uint16_t pixel = 0; pixel < scan_line_pixels; pixel++, base++) {
        const uint8_t *channel_ptr = base;
        for (uint8_t ch = 0; ch < total_channels; ch++) {
            uint8_t color = channel_ptr[(uint16_t)channel_pixels * ch];
            const _1_577_bsrr_t *entry = &bsrr[ch][color];
            pl_hub75_bsrr_flush(&entry->r);
            pl_hub75_bsrr_flush(&entry->g);
            pl_hub75_bsrr_flush(&entry->b);
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: RUC7258E 译码器型行驱动 ---- */
static void _1_577_set_row(uint8_t row)
{
    pl_hub75_Decoder_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t _1_577_ops = {
    .prepare = _1_577_prepare,
    .scan    = _1_577_scan,
    .set_row = _1_577_set_row,
};

/* ================================================================
 *  dev_display_1_577_init: 预计算 BSRR 查表 + 绑定 ops
 *
 *  g_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 * ================================================================ */

void dev_display_1_577_init(void)
{
    g_1_577.me.ops = &_1_577_ops;
    dev_display_register(&g_1_577.me);

    for (uint8_t ch = 0; ch < g_1_577.me.total_channels; ch++) {
        const uint32_t r_on  = (uint32_t)g_hub75_pin_r[ch].pin;
        const uint32_t g_on  = (uint32_t)g_hub75_pin_g[ch].pin;
        const uint32_t b_on  = (uint32_t)g_hub75_pin_b[ch].pin;
        const uint32_t r_off = r_on << 16;
        const uint32_t g_off = g_on << 16;
        const uint32_t b_off = b_on << 16;

        for (uint8_t color = 0; color < 8; color++) {
            g_bsrr[ch][color].r.port = g_hub75_pin_r[ch].port;
            g_bsrr[ch][color].r.val  = (color & 1U) ? r_on : r_off;
            g_bsrr[ch][color].g.port = g_hub75_pin_g[ch].port;
            g_bsrr[ch][color].g.val  = (color & 2U) ? g_on : g_off;
            g_bsrr[ch][color].b.port = g_hub75_pin_b[ch].port;
            g_bsrr[ch][color].b.val  = (color & 4U) ? b_on : b_off;
        }
    }
}
hw_dev_initcall(dev_display_1_577_init);
