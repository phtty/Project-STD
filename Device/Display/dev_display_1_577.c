/**
 * @file    dev_display_1_577.c
 * @brief   1-577 模组派生类型 — P5 户外全彩（左右半区映射）
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr / row_dst 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 *
 * 仅改下方模组宏即可驱动不同拼屏尺寸；映射规则仍为 1-577 左右半区。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  1-577 模组参数 — P5 户外全彩
 *
 *  单模块 64x32 像素，HUB75 2 通道/模块，ABC 1/8 扫描。
 *  改 MODULE_ROWS / MODULE_COLS 即可切换拼屏（如 1x1 → 64x32，3x3 → 192x96）。
 * ================================================================ */
#define MODULE_CODE                "1000000577"

#define _1_577_MODULE_ROWS         (1U)  /* 每行模块数（水平） */
#define _1_577_MODULE_COLS         (1U)  /* 每列模块数（垂直） */
#define _1_577_MODULE_PIXEL_ROW    (64U) /* 单模块每行的像素个数 */
#define _1_577_MODULE_PIXEL_COL    (32U) /* 单模块每列的像素个数 */
#define _1_577_CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define _1_577_SCAN_LINES          (8U)  /* 1/8 扫描 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define _1_577_SCREEN_ROWS    (_1_577_MODULE_ROWS * _1_577_MODULE_PIXEL_ROW) /* 屏幕每行像素数 */
#define _1_577_SCREEN_COLS    (_1_577_MODULE_COLS * _1_577_MODULE_PIXEL_COL) /* 屏幕每列像素数 */
#define _1_577_BUFFER_SIZE    (_1_577_SCREEN_ROWS * _1_577_SCREEN_COLS)
#define _1_577_TOTAL_CHANNELS (_1_577_MODULE_COLS * _1_577_CHANNELS_PER_MODULE)
#define _1_577_CHANNEL_PIXELS (_1_577_MODULE_PIXEL_ROW * _1_577_MODULE_PIXEL_COL * _1_577_MODULE_ROWS / _1_577_CHANNELS_PER_MODULE)
#define _1_577_SCAN_LINE_PX   (_1_577_CHANNEL_PIXELS / _1_577_SCAN_LINES)

/* ================================================================
 *  hub75_buff 布局（1-577 左右半区）
 *
 *  每通道内按 scan line 连续存放；每条 scan line 内按模块横向拼接：
 *    单模块 128 字节 = 左半 32 + 低/高间隔 32 + 右半 32 + 间隔 32
 *      [0..31]   = Y_high, X=0..31
 *      [32..63]  = Y_low,  X=0..31
 *      [64..95]  = Y_high, X=32..63
 *      [96..127] = Y_low,  X=32..63
 *    后续模块各占 MODULE_BUFF_STRIDE 字节。
 *
 *  行映射（1/8 扫描，每通道覆盖 ROWS_PER_CHANNEL 行）：
 *    local_y < SCAN_LINES → Y_low  （写入半区偏移 HALF_W）
 *    local_y >= SCAN_LINES → Y_high（写入半区偏移 0）
 * ================================================================ */

#define _1_577_HALF_W            (_1_577_MODULE_PIXEL_ROW / 2U)              /* 32：左右半宽 */
#define _1_577_MODULE_BUFF_STRIDE (_1_577_MODULE_PIXEL_ROW * 2U)             /* 128：单模块 scan 占用 */
#define _1_577_ROWS_PER_CHANNEL  (_1_577_MODULE_PIXEL_COL / _1_577_CHANNELS_PER_MODULE) /* 16 */

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
[[gnu::section(".ccmram")]] static uint16_t _1_577_row_dst[_1_577_SCREEN_COLS];

static dev_display_1_577_t g_1_577 = {
    .me = {
        .ops                 = nullptr, /* 由 dev_display_1_577_init 设置 */
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
        .light_level         = DEV_DISPLAY_BRIGHTNESS_MAX,
    },
};

dev_display_t *dev_display_1_577_get(void)
{
    return &g_1_577.me;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按 1-577 左右半区物理移位顺序组织。
 *  行起始偏移由 init 预计算到 _1_577_row_dst[]，此处只做拷贝。
 * ================================================================ */

static void _1_577_prepare(dev_display_t *dev)
{
    const uint16_t screen_rows = dev->screen_rows;
    const uint16_t screen_cols = dev->screen_cols;
    const uint8_t *pixel_map   = dev->pixel_map;
    uint8_t *hub75_buff        = dev->hub75_buff;

    for (uint16_t row = 0; row < screen_cols; row++) {
        const uint8_t *src = pixel_map + (uint16_t)(row * screen_rows);
        uint8_t *dst_base  = hub75_buff + _1_577_row_dst[row];

        for (uint16_t m = 0; m < _1_577_MODULE_ROWS; m++) {
            uint8_t *dst     = dst_base + m * _1_577_MODULE_BUFF_STRIDE;
            const uint8_t *s = src + m * _1_577_MODULE_PIXEL_ROW;

            for (uint16_t x = 0; x < _1_577_HALF_W; x++) {
                dst[x]                          = s[x];                  /* 左半 X=0..31 */
                dst[_1_577_MODULE_PIXEL_ROW + x] = s[_1_577_HALF_W + x]; /* 右半 X=32..63 */
            }
        }
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  hub75_buff[] 存储的是颜色索引 (0~7)，扫描时查 g_bsrr
 *  得到该通道、该颜色的 R/G/B 三组 {port, BSRR_val}，直接 flush 输出。
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
 *  dev_display_1_577_init: 预计算 row_dst / BSRR 查表 + 绑定 ops
 *
 *  _1_577_row_dst[y] = 该逻辑行在 hub75_buff 中的起始偏移（含高低半区）。
 *  g_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 * ================================================================ */

void dev_display_1_577_init(void)
{
    g_1_577.me.ops = &_1_577_ops;
    dev_display_register(&g_1_577.me);

    for (uint16_t row = 0; row < _1_577_SCREEN_COLS; row++) {
        const uint16_t ch       = (uint16_t)(row / _1_577_ROWS_PER_CHANNEL);
        const uint16_t local_y  = (uint16_t)(row % _1_577_ROWS_PER_CHANNEL);
        const uint16_t line     = (uint16_t)(local_y % _1_577_SCAN_LINES);
        const uint16_t row_type = (local_y < _1_577_SCAN_LINES) ? 1U : 0U; /* 1=low, 0=high */
        _1_577_row_dst[row] =
            (uint16_t)(ch * _1_577_CHANNEL_PIXELS + line * _1_577_SCAN_LINE_PX +
                       row_type * _1_577_HALF_W);
    }

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
