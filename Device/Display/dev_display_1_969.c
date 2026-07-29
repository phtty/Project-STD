/**
 * @file    dev_display_1_969.c
 * @brief   1-969 模组派生类型 — 1/8扫描、2 通道、R/G/B 分离
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  1-969 模组参数
 * ================================================================ */
#define MODULE_CODE                "1000000969"

#define _1_969_MODULE_ROWS         (3U)  /* 每行模块数 */
#define _1_969_MODULE_COLS         (3U)  /* 每列模块数 */
#define _1_969_MODULE_PIXEL_ROW    (64U) /* 单模块每行的像素个数 */
#define _1_969_MODULE_PIXEL_COL    (32U) /* 单模块每列的像素个数 */
#define _1_969_CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define _1_969_SCAN_LINES          (8U)  /* 1/8 扫描 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define _1_969_SCREEN_ROWS    (_1_969_MODULE_ROWS * _1_969_MODULE_PIXEL_ROW) // 屏幕每行的像素个数
#define _1_969_SCREEN_COLS    (_1_969_MODULE_COLS * _1_969_MODULE_PIXEL_COL) // 屏幕每列的像素个数
#define _1_969_BUFFER_SIZE    (_1_969_SCREEN_ROWS * _1_969_SCREEN_COLS)
#define _1_969_TOTAL_CHANNELS (_1_969_MODULE_COLS * _1_969_CHANNELS_PER_MODULE)
#define _1_969_CHANNEL_PIXELS (_1_969_MODULE_PIXEL_ROW * _1_969_MODULE_PIXEL_COL * _1_969_MODULE_ROWS / _1_969_CHANNELS_PER_MODULE)
#define _1_969_SCAN_LINE_PX   (_1_969_CHANNEL_PIXELS / _1_969_SCAN_LINES)

/* ================================================================
 *  hub75_buff 布局
 *
 * HUB75 协议以"组"为单位移位输出，每组 16 字节对应 4×4 像素块。
 * 屏幕 16×16 像素 → 4×4 组，每组覆盖 4 行 × 4 列。
 *
 * 每个组 16 字节 = 4 行 × 4 列，每组内布局：
 *   偏移  row%4  col: 0   1   2   3
 *   ───── ─────  ───────────────────
 *    0..3    0       6   4   2   0   ← 行镜像 (3 - col)
 *    4..7    1       7   5   3   1   ← 行镜像 + 偏移 1
 *    8..11   2       8  10  12  14   ← 行正序 + 偏移 9
 *   12..15   3       9  11  13  15   ← 行正序 + 偏移 8
 *
 * 每组 2 字节一组（对应 R/G/B 三色），每组内 4 行的偏移规律
 * 由 HUB75 驱动芯片的数据输入格式决定。
 * ================================================================ */

#define GROUP_PIXEL_W 4  /* 每组覆盖的列数 */
#define GROUP_PIXEL_H 4  /* 每组覆盖的行数 */
#define GROUP_SIZE    16 /* 每组的字节数 = GROUP_PIXEL_W × GROUP_PIXEL_H */

/* 组内行偏移的基准值 */
#define GROUP_ROW0_OFFSET  0 /* row%4 == 0: 双字节对偏移 0 */
#define GROUP_ROW1_OFFSET  1 /* row%4 == 1: 双字节对偏移 1 */
#define GROUP_ROW2_OFFSET  9 /* row%4 == 2: 双字节对偏移 9 */
#define GROUP_ROW3_OFFSET  8 /* row%4 == 3: 双字节对偏移 8 */

#define GROUP_COLS_PER_ROW (_1_969_MODULE_ROWS * GROUP_PIXEL_W) /* 每行模块的水平组数 */

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} _1_969_bsrr_t;

[[gnu::section(".ccmram")]] static _1_969_bsrr_t g_bsrr[_1_969_TOTAL_CHANNELS][8];

/* ---- 1-969 实例 ---- */
typedef struct {
    dev_display_t me;
} dev_display_1_969_t;

[[gnu::section(".ccmram")]] static uint8_t _1_969_pixel_map[_1_969_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t _1_969_hub75_buff[_1_969_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint16_t _1_969_row_dst[_1_969_SCREEN_COLS];

static dev_display_1_969_t g_1_969 = {
    .me = {
        .ops                 = nullptr, /* 由 dev_display_1_969_init 设置 */
        .module_rows         = _1_969_MODULE_PIXEL_ROW,
        .module_cols         = _1_969_MODULE_PIXEL_COL,
        .channels_per_module = _1_969_CHANNELS_PER_MODULE,
        .modules_per_row     = _1_969_MODULE_ROWS,
        .modules_per_col     = _1_969_MODULE_COLS,
        .scan_lines          = _1_969_SCAN_LINES,
        .screen_rows         = _1_969_SCREEN_ROWS,
        .screen_cols         = _1_969_SCREEN_COLS,
        .total_channels      = _1_969_TOTAL_CHANNELS,
        .channel_pixels      = _1_969_CHANNEL_PIXELS,
        .scan_line_pixels    = _1_969_SCAN_LINE_PX,
        .buffer_size         = _1_969_BUFFER_SIZE,
        .pixel_map           = _1_969_pixel_map,
        .hub75_buff          = _1_969_hub75_buff,
        .module_code         = MODULE_CODE,
        .light_level         = 7,
    },
};

dev_display_t *dev_display_1_969_get(void)
{
    return &g_1_969.me;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按"组"组织以匹配 HUB75 移位寄存器输入时序。
 *  此函数将像素值从逻辑坐标映射到硬件输出缓冲。
 * ================================================================ */

static void _1_969_prepare(dev_display_t *dev)
{
    const uint16_t screen_rows = dev->screen_rows;
    const uint16_t screen_cols = dev->screen_cols;
    const uint8_t *pixel_map   = dev->pixel_map;
    uint8_t *hub75_buff        = dev->hub75_buff;

    for (uint16_t row = 0; row < screen_cols; row++) {
        const uint8_t *src = pixel_map + (uint16_t)(row * screen_rows);
        uint8_t *dst = hub75_buff + _1_969_row_dst[row];

        for (uint16_t col = 0; col < screen_rows; col++) {
            *dst = *src++;
            dst += 2U;
        }
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  hub75_buff[] 存储的是颜色索引 (0~7)，扫描时查 g_bsrr
 *  得到该通道、该颜色的 R/G/B 三组 {port, BSRR_val}，直接 flush 输出。
 * ================================================================ */

static inline void _1_969_scan(dev_display_t *dev, uint8_t line)
{
    const uint16_t scan_line_pixels = dev->scan_line_pixels;
    const uint16_t channel_pixels   = dev->channel_pixels;
    const uint8_t total_channels    = dev->total_channels;
    const _1_969_bsrr_t (*bsrr)[8]  = g_bsrr;
    const uint8_t *base             = dev->hub75_buff + (uint16_t)line * scan_line_pixels;

    for (uint16_t pixel = 0; pixel < scan_line_pixels; pixel++, base++) {
        const uint8_t *channel_ptr = base;
        const uint8_t color0 = channel_ptr[0];
        const uint8_t color1 = channel_ptr[channel_pixels];
        const uint8_t color2 = channel_ptr[(uint16_t)channel_pixels * 2U];
        const uint8_t color3 = channel_ptr[(uint16_t)channel_pixels * 3U];
        const uint8_t color4 = channel_ptr[(uint16_t)channel_pixels * 4U];
        const uint8_t color5 = channel_ptr[(uint16_t)channel_pixels * 5U];

        const _1_969_bsrr_t *entry0 = &bsrr[0][color0];
        const _1_969_bsrr_t *entry1 = &bsrr[1][color1];
        const _1_969_bsrr_t *entry2 = &bsrr[2][color2];
        const _1_969_bsrr_t *entry3 = &bsrr[3][color3];
        const _1_969_bsrr_t *entry4 = &bsrr[4][color4];
        const _1_969_bsrr_t *entry5 = &bsrr[5][color5];

        pl_hub75_bsrr_flush(&entry0->r);
        pl_hub75_bsrr_flush(&entry0->g);
        pl_hub75_bsrr_flush(&entry0->b);
        pl_hub75_bsrr_flush(&entry1->r);
        pl_hub75_bsrr_flush(&entry1->g);
        pl_hub75_bsrr_flush(&entry1->b);
        pl_hub75_bsrr_flush(&entry2->r);
        pl_hub75_bsrr_flush(&entry2->g);
        pl_hub75_bsrr_flush(&entry2->b);
        pl_hub75_bsrr_flush(&entry3->r);
        pl_hub75_bsrr_flush(&entry3->g);
        pl_hub75_bsrr_flush(&entry3->b);
        pl_hub75_bsrr_flush(&entry4->r);
        pl_hub75_bsrr_flush(&entry4->g);
        pl_hub75_bsrr_flush(&entry4->b);
        pl_hub75_bsrr_flush(&entry5->r);
        pl_hub75_bsrr_flush(&entry5->g);
        pl_hub75_bsrr_flush(&entry5->b);

        (void)total_channels;
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: RUC7258E 译码器型行驱动 ---- */
static void _1_969_set_row(uint8_t row)
{
    pl_hub75_Decoder_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t _1_969_ops = {
    .prepare = _1_969_prepare,
    .scan    = _1_969_scan,
    .set_row = _1_969_set_row,
};

/* ================================================================
 *  dev_display_1_969_init: 预计算 BSRR 查表 + 绑定 ops
 *
 *  g_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 *  颜色 c 的 bit0→R, bit1→G, bit2→B:
 *    - R 亮: (c & 1) != 0 → BSRR 置位（高电平）
 *    - R 灭: (c & 1) == 0 → BSRR 复位（低电平）
 *    - G 亮: (c & 2) != 0,  B 亮: (c & 4) != 0
 *
 *  预计算避免扫描热路径中的分支判断。
 * ================================================================ */

void dev_display_1_969_init(void)
{
    g_1_969.me.ops = &_1_969_ops;
    dev_display_register(&g_1_969.me);

    for (uint16_t row = 0; row < _1_969_SCREEN_COLS; row++) {
        const uint16_t group_index = (uint16_t)((row >> 4) * 8U + (row & 7U));
        const uint16_t row_base = (uint16_t)(group_index * _1_969_SCAN_LINE_PX);
        const uint8_t row_offset = (uint8_t)(((row >> 3) & 1U) ? 0U : 1U);
        _1_969_row_dst[row] = (uint16_t)(row_base + row_offset);
    }

    for (uint8_t ch = 0; ch < g_1_969.me.total_channels; ch++) {
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
hw_dev_initcall(dev_display_1_969_init);
