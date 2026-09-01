/**
 * @file    dev_display_1_260.c
 * @brief   1-260 模组派生类型 — 1/8扫描、2 通道、R/G/B 分离 — SM16026S+SM5266PH
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  1-260 模组参数
 * ================================================================ */
#define MODULE_CODE                "1000000260"

#define _1_260_MODULE_ROWS         (1U)  /* 每行模块数 */
#define _1_260_MODULE_COLS         (1U)  /* 每列模块数 */
#define _1_260_MODULE_PIXEL_ROW    (32U) /* 单模块每行的像素个数 */
#define _1_260_MODULE_PIXEL_COL    (32U) /* 单模块每列的像素个数 */
#define _1_260_CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define _1_260_SCAN_LINES          (8U)  /* 1/8 扫描 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define _1_260_SCREEN_ROWS    (_1_260_MODULE_ROWS * _1_260_MODULE_PIXEL_ROW) // 屏幕每行的像素个数
#define _1_260_SCREEN_COLS    (_1_260_MODULE_COLS * _1_260_MODULE_PIXEL_COL) // 屏幕每列的像素个数
#define _1_260_BUFFER_SIZE    (_1_260_SCREEN_ROWS * _1_260_SCREEN_COLS)
#define _1_260_TOTAL_CHANNELS (_1_260_MODULE_COLS * _1_260_CHANNELS_PER_MODULE)
#define _1_260_CHANNEL_PIXELS (_1_260_MODULE_PIXEL_ROW * _1_260_MODULE_PIXEL_COL * _1_260_MODULE_ROWS / _1_260_CHANNELS_PER_MODULE)
#define _1_260_SCAN_LINE_PX   (_1_260_CHANNEL_PIXELS / _1_260_SCAN_LINES)

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

#define GROUP_COLS_PER_ROW (_1_260_MODULE_ROWS * GROUP_PIXEL_W) /* 每行模块的水平组数 */

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} _1_260_bsrr_t;

[[gnu::section(".ccmram")]] static _1_260_bsrr_t g_bsrr[_1_260_TOTAL_CHANNELS][8];

/* ---- 1-260 实例 ---- */
typedef struct {
    dev_display_t me;
} dev_display_1_260_t;

[[gnu::section(".ccmram")]] static uint8_t _1_260_pixel_map[_1_260_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t _1_260_hub75_buff[_1_260_BUFFER_SIZE];

static dev_display_1_260_t g_1_260 = {
    .me = {
        .ops                 = nullptr, /* 由 dev_display_1_260_init 设置 */
        .module_rows         = _1_260_MODULE_PIXEL_ROW,
        .module_cols         = _1_260_MODULE_PIXEL_COL,
        .channels_per_module = _1_260_CHANNELS_PER_MODULE,
        .modules_per_row     = _1_260_MODULE_ROWS,
        .modules_per_col     = _1_260_MODULE_COLS,
        .scan_lines          = _1_260_SCAN_LINES,
        .screen_rows         = _1_260_SCREEN_ROWS,
        .screen_cols         = _1_260_SCREEN_COLS,
        .total_channels      = _1_260_TOTAL_CHANNELS,
        .channel_pixels      = _1_260_CHANNEL_PIXELS,
        .scan_line_pixels    = _1_260_SCAN_LINE_PX,
        .buffer_size         = _1_260_BUFFER_SIZE,
        .pixel_map           = _1_260_pixel_map,
        .hub75_buff          = _1_260_hub75_buff,
        .module_code         = MODULE_CODE,
        .light_level         = DEV_DISPLAY_BRIGHTNESS_MAX,
    },
};

dev_display_t *dev_display_1_260_get(void)
{
    return &g_1_260.me;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按"组"组织以匹配 HUB75 移位寄存器输入时序。
 *  此函数将像素值从逻辑坐标映射到硬件输出缓冲。
 * ================================================================ */

static void _1_260_prepare(dev_display_t *dev)
{
    for (uint16_t linear = 0; linear < dev->buffer_size; linear++) {
        uint16_t x              = linear % dev->screen_rows;
        uint16_t y              = linear / dev->screen_rows;
        dev->hub75_buff[linear] = dev->pixel_map[y * dev->screen_rows + x];
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  hub75_buff[] 存储的是颜色索引 (0~7)，扫描时查 g_bsrr
 *  得到该通道、该颜色的 R/G/B 三组 {port, BSRR_val}，直接 flush 输出。
 * ================================================================ */

static inline void _1_260_scan(dev_display_t *dev, uint8_t line)
{
    for (uint16_t pixel = 0; pixel < dev->scan_line_pixels; pixel++) {
        /* 当前像素在所有通道中的起始偏移 */
        uint16_t pixel_base = (uint16_t)line * dev->scan_line_pixels + pixel;

        /* 同一像素位置同时输出所有通道的颜色数据 */
        for (uint8_t ch = 0; ch < dev->total_channels; ch++) {
            uint8_t color       = dev->hub75_buff[pixel_base + ch * dev->channel_pixels];
            _1_260_bsrr_t *bsrr = &g_bsrr[ch][color];
            pl_hub75_bsrr_flush(&bsrr->r);
            pl_hub75_bsrr_flush(&bsrr->g);
            pl_hub75_bsrr_flush(&bsrr->b);
        }

        /* 锁存当前像素数据到移位寄存器 */
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: SM5266PH 移位寄存器型行驱动 ---- */
static void _1_260_set_row(uint8_t row)
{
    pl_hub75_ShiftRegister_set_row(row, 1);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t _1_260_ops = {
    .prepare = _1_260_prepare,
    .scan    = _1_260_scan,
    .set_row = _1_260_set_row,
};

/* ================================================================
 *  dev_display_1_260_init: 预计算 BSRR 查表 + 绑定 ops
 *
 *  g_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 *  颜色 c 的 bit0→R, bit1→G, bit2→B:
 *    - R 亮: (c & 1) != 0 → BSRR 置位（高电平）
 *    - R 灭: (c & 1) == 0 → BSRR 复位（低电平）
 *    - G 亮: (c & 2) != 0,  B 亮: (c & 4) != 0
 *
 *  预计算避免扫描热路径中的分支判断。
 * ================================================================ */

void dev_display_1_260_init(void)
{
    g_1_260.me.ops = &_1_260_ops;
    dev_display_register(&g_1_260.me);

    for (uint8_t ch = 0; ch < g_1_260.me.total_channels; ch++) {
        for (uint8_t color = 0; color < 8; color++) {
            /* R 通道: color bit0 决定亮灭 */
            g_bsrr[ch][color].r.port = g_hub75_pin_r[ch].port;
            g_bsrr[ch][color].r.val  = (color & 1) ? (uint32_t)g_hub75_pin_r[ch].pin        /* 置位: 输出高 */
                                                   : (uint32_t)g_hub75_pin_r[ch].pin << 16; /* 复位: 输出低 */

            /* G 通道: color bit1 决定亮灭 */
            g_bsrr[ch][color].g.port = g_hub75_pin_g[ch].port;
            g_bsrr[ch][color].g.val  = (color & 2) ? (uint32_t)g_hub75_pin_g[ch].pin
                                                   : (uint32_t)g_hub75_pin_g[ch].pin << 16;

            /* B 通道: color bit2 决定亮灭 */
            g_bsrr[ch][color].b.port = g_hub75_pin_b[ch].port;
            g_bsrr[ch][color].b.val  = (color & 4) ? (uint32_t)g_hub75_pin_b[ch].pin
                                                   : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }
}
hw_dev_initcall(dev_display_1_260_init);