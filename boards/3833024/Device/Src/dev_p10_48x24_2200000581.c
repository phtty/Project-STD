/**
 * @file    dev_p10_48x24_2200000581.c
 * @brief   48×24 / 1/4 扫 HUB75 模组 —— 一块卡带 N 个模组
 *
 * 单模组的灯序（实测得出）：
 *  - 地址值（A/B）= 组内第几行，组号（哪 4 行）由移位位置决定
 *  - 每 8 步 = 一个行组 × 8 列；先走行 8-15（两组），再走行 0-7（两组）
 *  - 偶数行组列序正序、奇数行组 8 像素组内反序
 *  - 第二个 RGB 组（8 行）只有 96 位，放在 step 96..191，避免被 192 个时钟移出
 *
 * 多模组（本产品 2 行 3 列 = 6 个，主卡带右侧 2×2、从卡带左侧 2×1）：
 *  - 所有模组共用 CLK / LAT / OE / A-B，每个模组占一对 RGB 数据线
 *  - 模组序号按**列优先**：m = 列 * ROWS + 行（与现场接线一致）
 *  - 通道 ch = 2m + 组号（组 0 = 上 16 行，组 1 = 下 8 行）
 *  - 同一 step 上每个模组输出各自缓冲里的同一位置，然后统一打一个 CLK
 *
 * 缓冲布局：idx = (line * STEPS_PER_LINE + step) * CHANNELS + ch
 */

#include "dev_display.h"

#include <stdio.h>
#include <string.h>
#include "initcall.h"

/* ================================================================
 *  模组与整卡参数
 * ================================================================ */

#define MOD_PIX_W      (48U) /* 单模组像素列数 */
#define MOD_PIX_H      (24U) /* 单模组像素行数 */
#define MOD_CH0_ROWS   (16U) /* 第一组 RGB 负责的行数（上 16 行） */
#define MOD_CH1_ROWS   (8U)  /* 第二组 RGB 负责的行数（下 8 行） */

#define MOD_SCAN_LINES (4U)  /* 1/4 扫，A/B 选行 */

/* 每个扫描线要发多少个时钟：模组第一组 4 行 × 48 列 = 192 */
#define STEPS_PER_LINE (192U)

/* 每卡模组数（板级 board.h 配置，主卡 2×2=4、从卡 1×2=2） */
#define MOD_COLS       ((uint8_t)BOARD_DISPLAY_MODULE_COLS)
#define MOD_ROWS       ((uint8_t)BOARD_DISPLAY_MODULE_ROWS)
#define MOD_COUNT      ((uint8_t)(MOD_COLS * MOD_ROWS))
#define CHANNELS       ((uint8_t)(MOD_COUNT * 2U))

#define SCREEN_W       ((uint16_t)(MOD_COLS * MOD_PIX_W))
#define SCREEN_H       ((uint16_t)(MOD_ROWS * MOD_PIX_H))
#define BUFF_SIZE      ((uint32_t)(MOD_SCAN_LINES * STEPS_PER_LINE * CHANNELS))

/* ================================================================
 *  颜色 → BSRR 预计算表（CHANNELS × 8 色）
 * ================================================================ */

typedef struct {
    pl_hub75_bsrr_t r, g, b;
} hub75_bsrr_t;

[[gnu::section(".ccmram")]] static hub75_bsrr_t s_bsrr[CHANNELS][8];

/* ---- 实例 ---- */
typedef struct {
    dev_display_t base;
} dev_display_hub75_48x24_t;

[[gnu::section(".ccmram")]] static uint8_t s_pixel_map[SCREEN_W * SCREEN_H];
[[gnu::section(".ccmram")]] static uint8_t s_hub75_buff[BUFF_SIZE];

static dev_display_hub75_48x24_t s_dev = {
    .base = {
        .ops = nullptr, /* 由 init 绑定 */
        /* 本工程的命名：module_rows/screen_rows 是**水平方向**（宽），
         * module_cols/screen_cols 是垂直方向（高）。P20 的 screen_rows=128 即宽。 */
        .module_rows          = MOD_PIX_W,
        .module_cols          = MOD_PIX_H,
        .channels_per_module  = 2U,
        .modules_per_row      = MOD_COLS,
        .modules_per_col      = MOD_ROWS,
        .scan_lines           = MOD_SCAN_LINES,
        /* 本模组只有红/绿两种灯珠：黑/红/绿/黄，不含蓝系 */
        .supported_color_mask = DEV_DISPLAY_COLOR_ALL
            & ~(DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_BLUE)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_PURPLE)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_CYAN)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_WHITE)),
        .screen_rows       = SCREEN_W,
        .screen_cols       = SCREEN_H,
        .total_channels    = CHANNELS,
        .channel_pixels    = (uint16_t)(MOD_SCAN_LINES * STEPS_PER_LINE), /* 单通道 768，仅参考 */
        .scan_line_pixels  = STEPS_PER_LINE,
        .buffer_size       = (uint16_t)BUFF_SIZE,
        .pixel_map         = s_pixel_map,
        .hub75_buff        = s_hub75_buff,
        .light_level       = 1,
        .test_step         = -1, /* 默认正常扫描 */
    },
};

dev_display_t *dev_display_hub75_48x24_get(void)
{
    return &s_dev.base;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff
 * ================================================================ */

static void _prepare(dev_display_t *dev)
{
    memset(dev->hub75_buff, 0, dev->buffer_size);

    for (uint16_t y = 0; y < SCREEN_H; y++) {
        for (uint16_t x = 0; x < SCREEN_W; x++) {
            const uint8_t col = (uint8_t)(x / MOD_PIX_W);
            const uint8_t row = (uint8_t)(y / MOD_PIX_H);
            const uint8_t m   = (uint8_t)(col * MOD_ROWS + row); /* 列优先，与接线一致 */

            const uint16_t mx = (uint16_t)(x % MOD_PIX_W);
            const uint16_t my = (uint16_t)(y % MOD_PIX_H);

            const uint8_t block = (uint8_t)(mx / 8U); /* 0..5，每块 8 列 */
            const uint8_t cw    = (uint8_t)(mx % 8U); /* 块内列 0..7 */

            uint8_t  line;
            uint16_t step;
            uint8_t  ch_off;

            if (my < MOD_CH0_ROWS) {
                const uint8_t rg   = (uint8_t)(my / 4U);      /* 0..3 */
                const uint8_t half = (rg >= 2U) ? 0U : 1U;   /* 先 2/3，再 0/1 */
                const uint8_t c    = (uint8_t)(half * 12U + block * 2U + (rg & 1U));
                /* 奇数行组（4-7、12-15）的 8 像素组内列序相反 */
                const uint8_t cw_e = (rg & 1U) ? (uint8_t)(7U - cw) : cw;
                line               = (uint8_t)(my % 4U);
                step               = (uint16_t)(c * 8U + cw_e);
                ch_off             = 0U;
            } else {
                const uint8_t yi   = (uint8_t)(my - MOD_CH0_ROWS); /* 0..7 */
                const uint8_t rg   = (uint8_t)(yi / 4U);           /* 0..1 */
                const uint8_t c    = (uint8_t)(block * 2U + rg);
                /* 同理：奇数行组（20-23）组内列序相反 */
                const uint8_t cw_e = (rg & 1U) ? (uint8_t)(7U - cw) : cw;
                line               = (uint8_t)(yi % 4U);
                /* 组 1 只有 96 位，放在后 96 步，避免被 192 个时钟移出寄存器 */
                step               = (uint16_t)(96U + c * 8U + cw_e);
                ch_off             = 1U;
            }

            const uint8_t  ch  = (uint8_t)(m * 2U + ch_off);
            const uint32_t idx = ((uint32_t)line * STEPS_PER_LINE + step) * CHANNELS + ch;
            dev->hub75_buff[idx] = dev->pixel_map[(uint32_t)y * SCREEN_W + x];
        }
    }
}

/* ================================================================
 *  scan: 按物理移位顺序输出一条扫描线（所有模组同步）
 * ================================================================ */

/* 灯序测试的历史轨迹：1 = 已点亮过（显示红色） */
[[gnu::section(".ccmram")]] static uint8_t s_trail[STEPS_PER_LINE * CHANNELS];
static int16_t                     s_trail_step = -1;
static uint8_t                     s_trail_ch;

static void _scan(dev_display_t *dev, uint8_t line)
{
    const uint32_t base   = (uint32_t)line * STEPS_PER_LINE * CHANNELS;
    const bool     test   = (dev->test_step >= 0);
    const uint16_t t_step = (uint16_t)dev->test_step;

    if (test) {
        /* 测试点变了 → 把上一个点记进轨迹（下一帧起显示红色） */
        if (s_trail_step >= 0 &&
            (s_trail_step != dev->test_step || s_trail_ch != dev->test_ch)) {
            s_trail[(uint16_t)s_trail_step * CHANNELS + s_trail_ch] = 1U;
        }
        s_trail_step = dev->test_step;
        s_trail_ch   = dev->test_ch;
    } else if (s_trail_step >= 0) {
        memset(s_trail, 0, sizeof(s_trail));
        s_trail_step = -1;
        s_trail_ch   = 0;
    }

    for (uint16_t step = 0; step < STEPS_PER_LINE; step++) {
        for (uint8_t ch = 0; ch < CHANNELS; ch++) {
            uint8_t color;
            if (test) {
                const uint32_t ti = (uint32_t)step * CHANNELS + ch;
                if (step == t_step && ch == dev->test_ch)
                    color = DEV_DISPLAY_COLOR_GREEN; /* 当前点 */
                else if (s_trail[ti])
                    color = DEV_DISPLAY_COLOR_RED; /* 亮过的点 */
                else
                    color = DEV_DISPLAY_COLOR_BLACK;
            } else {
                color = dev->hub75_buff[base + (uint32_t)step * CHANNELS + ch];
            }
            const hub75_bsrr_t *b = &s_bsrr[ch][color];
            pl_hub75_bsrr_flush(&b->r);
            pl_hub75_bsrr_flush(&b->g);
            pl_hub75_bsrr_flush(&b->b);
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: 1/4 扫，只有 A/B 两根地址线参与 ---- */
static void _set_row(uint8_t line)
{
    pl_hub75_set_row((uint8_t)(line & 0x03U));
}

static const dev_display_ops_t s_ops = {
    .prepare = _prepare,
    .scan    = _scan,
    .set_row = _set_row,
};

/* ================================================================
 *  init: 预计算 BSRR 表 + 注册实例
 * ================================================================ */
void dev_p10_48x24_2200000581_init(void)
{
    s_dev.base.ops = &s_ops;
    dev_display_register(&s_dev.base);

    printf("[disp] p10_48x24 %ux%u (%ux%u 模组, %u 通道)\n", (unsigned)SCREEN_W,
           (unsigned)SCREEN_H, (unsigned)MOD_COLS, (unsigned)MOD_ROWS, (unsigned)CHANNELS);

    for (uint8_t ch = 0; ch < CHANNELS; ch++) {
        for (uint8_t color = 0; color < 8; color++) {
            s_bsrr[ch][color].r.port = g_hub75_pin_r[ch].port;
            s_bsrr[ch][color].r.val  = (color & 1U) ? (uint32_t)g_hub75_pin_r[ch].pin
                                                    : (uint32_t)g_hub75_pin_r[ch].pin << 16;
            s_bsrr[ch][color].g.port = g_hub75_pin_g[ch].port;
            s_bsrr[ch][color].g.val  = (color & 2U) ? (uint32_t)g_hub75_pin_g[ch].pin
                                                    : (uint32_t)g_hub75_pin_g[ch].pin << 16;
            s_bsrr[ch][color].b.port = g_hub75_pin_b[ch].port;
            s_bsrr[ch][color].b.val  = (color & 4U) ? (uint32_t)g_hub75_pin_b[ch].pin
                                                    : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }
}
hw_dev_initcall(dev_p10_48x24_2200000581_init);
