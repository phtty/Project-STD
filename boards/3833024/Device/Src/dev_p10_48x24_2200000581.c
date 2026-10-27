/**
 * @file    dev_hub75_48x24_1of4.c
 * @brief   48×24 / 1/4 扫 HUB75 模组 —— 行扫描骨架
 *
 * 与 P10/P20 驱动的差别（也是本文件存在的理由）：
 *
 *  1. **两组 RGB 承担的像素行数不相等**：R1G1B1 管上 16 行，R2G2B2 管下 8 行。
 *     引擎基类里 `channel_pixels` 是"每个通道像素数相同"的假设（24×48/2=576），
 *     在这里不成立（ch0=768、ch1=384），因此本驱动**不使用**它做通道基址，
 *     改用本文件自己的 STEPS_PER_LINE / CHANNELS 计算。
 *
 *  2. **两条通道共用 CLK/LAT/A/B**，每个扫描线必须发满 192 个时钟（ch0 的
 *     4 行 × 48 列）。ch1 只需要 96 个（2 行 × 48 列），后 96 个时钟对它是
 *     "移位溢出"：位数从它较短的寄存器末端流出，不会落到可见像素上。
 *     所以缓冲里 ch1 的后半段写 0 即可。
 *
 *  3. 行地址只有 A/B 两根（1/4 扫），由引擎的 `_scan_task` 在 OE 消隐窗口内
 *     调 `set_row()` 切换，亮度 PWM 也由引擎的 TIM4 中断负责 —— 本驱动不碰这两件事。
 *
 * 缓冲布局（hub75_buff）：
 *
 *     idx = (line * STEPS_PER_LINE + step) * CHANNELS + ch
 *
 *     下标含义：line = 扫描线 0..3（A/B 选中的一行组）
 *               step = 该扫描线内的第几个时钟（0..191）
 *               ch   = 0 = R1G1B1（上 16 行），1 = R2G2B2（下 8 行）
 *
 * 这个布局的好处：**线性遍历 hub75_buff 就是物理移位顺序**，
 * 所以灯序映射测试可以直接按 idx 递增点亮，不需要经过 prepare。
 *
 * ⚠️ 待确认：行/列到 step 的具体排列（哪一行先移、列是否需要镜像）
 *    取决于模组内部走线。当前 prepare 用的是"行 y 映射到扫描线 y%4、
 *    组内序号 y/4"的假设，必须用灯序测试确认后再改。
 */

#include "dev_display.h"

#include <stdio.h>
#include <string.h>
#include "initcall.h"

/* ================================================================
 *  模组参数
 * ================================================================ */

#define MOD_COLS       (48U) /* 每模块像素列数 */
#define MOD_ROWS       (24U) /* 每模块像素行数 */
#define MOD_CHANNELS   (2U)  /* R1G1B1 + R2G2B2 */
#define MOD_SCAN_LINES (4U)  /* 1/4 扫，A/B 选行 */

/* 两组 RGB 承担的行数（不相等，这是本模组的关键特征） */
#define CH0_ROWS          (16U)                       /* R1G1B1：上 16 行 */
#define CH1_ROWS          (8U)                        /* R2G2B2：下 8 行 */
#define CH0_ROWS_PER_LINE (CH0_ROWS / MOD_SCAN_LINES) /* 4 行/扫描线 */
#define CH1_ROWS_PER_LINE (CH1_ROWS / MOD_SCAN_LINES) /* 2 行/扫描线 */

/* 每个扫描线要发多少个时钟 = 较"长"的那条通道一行组的位数 */
#define STEPS_PER_LINE     (CH0_ROWS_PER_LINE * MOD_COLS) /* 192 */
#define CH1_STEPS_PER_LINE (CH1_ROWS_PER_LINE * MOD_COLS) /* 96（其余 96 步补 0） */

#define BUFF_SIZE          (MOD_SCAN_LINES * STEPS_PER_LINE * MOD_CHANNELS) /* 1536 */

/* 基类字段：引擎只用到 screen_rows / screen_cols / scan_lines / buffer_size；
 * channel_pixels 与 scan_line_pixels 本驱动不用于寻址，填成"最大通道"的值
 * 以免上层按它们算越界（见文件头说明）。 */
#define SCREEN_PIXELS (MOD_ROWS * MOD_COLS)             /* 1152 */
#define CH_PIXELS_MAX (MOD_SCAN_LINES * STEPS_PER_LINE) /* 768 = ch0 的全部像素 */

/* ---- 颜色 → BSRR 预计算表（2 通道 × 8 色） ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} hub75_bsrr_t;

[[gnu::section(".ccmram")]] static hub75_bsrr_t s_bsrr[MOD_CHANNELS][8];

/* ---- 实例 ---- */
typedef struct {
    dev_display_t base;
} dev_display_hub75_48x24_t;

[[gnu::section(".ccmram")]] static uint8_t s_pixel_map[SCREEN_PIXELS];
[[gnu::section(".ccmram")]] static uint8_t s_hub75_buff[BUFF_SIZE];

static dev_display_hub75_48x24_t s_dev = {
    .base = {
        .ops                  = nullptr, /* 由 init 绑定 */
        /* 注意本工程的命名：module_rows/screen_rows 是**水平方向**（宽），
         * module_cols/screen_cols 是垂直方向（高）。P20 的 screen_rows=128 即宽。 */
        .module_rows          = MOD_COLS,
        .module_cols          = MOD_ROWS,
        .channels_per_module  = MOD_CHANNELS,
        .modules_per_row      = 1U,
        .modules_per_col      = 1U,
        .scan_lines           = MOD_SCAN_LINES,
        /* 本模组只有红/绿两种灯珠：黑/红/绿/黄，不含蓝系 */
        .supported_color_mask = DEV_DISPLAY_COLOR_ALL
            & ~(DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_BLUE)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_PURPLE)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_CYAN)
              | DEV_DISPLAY_COLOR_BIT(DEV_DISPLAY_COLOR_WHITE)),
        .screen_rows          = MOD_COLS,
        .screen_cols          = MOD_ROWS,
        .total_channels       = MOD_CHANNELS,
        .channel_pixels       = CH_PIXELS_MAX,
        .scan_line_pixels     = STEPS_PER_LINE,
        .buffer_size          = BUFF_SIZE,
        .pixel_map            = s_pixel_map,
        .hub75_buff           = s_hub75_buff,
        .light_level          = 1,
        .test_step            = -1, /* 默认正常扫描 */
    },
};

dev_display_t *dev_display_hub75_48x24_get(void)
{
    return &s_dev.base;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff
 *
 *  结构（由灯序实测反推）：
 *
 *    每一个移位位置（step）同时点亮**同一组的连续 4 行**、同一列：
 *      地址值 a = 组内第几行（a=0 最上）
 *      组号（哪 4 行）= 由 step 决定
 *
 *    组按两两成对处理：先 (行8-15) 两组，再 (行0-7) 两组；
 *    每对里 6 个 8 列块，块内先偶数组再奇数组，每块 8 步：
 *
 *      step 0..7    → 行8-11, 列 0-7
 *      step 8..15   → 行12-15, 列 0-7
 *      step 16..23  → 行8-11, 列 8-15
 *      ...
 *      step 88..95  → 行12-15, 列 40-47
 *      step 96..103 → 行0-3,  列 0-7     （第二对开始）
 *      ...
 *      step 184..191→ 行4-7,  列 40-47
 *
 *    合计 24 个 chunk × 8 步 = 192 步 ✓
 *
 *  因此：line（地址）= y % 4，组号编码在 step 里。
 * ================================================================ */

static void _prepare(dev_display_t *dev)
{
    memset(dev->hub75_buff, 0, dev->buffer_size);

    for (uint16_t y = 0; y < MOD_ROWS; y++) {
        for (uint16_t x = 0; x < MOD_COLS; x++) {
            const uint8_t block = (uint8_t)(x / 8U); /* 0..5，每块 8 列 */
            const uint8_t cw    = (uint8_t)(x % 8U); /* 块内列 0..7 */

            uint8_t  line;
            uint16_t step;
            uint8_t  ch;

            if (y < CH0_ROWS) {
                const uint8_t rg   = (uint8_t)(y / 4U);      /* 0..3 */
                const uint8_t half = (rg >= 2U) ? 0U : 1U;   /* 先 2/3，再 0/1 */
                const uint8_t c    = (uint8_t)(half * 12U + block * 2U + (rg & 1U));
                /* 奇数行组（4-7、12-15）的 8 像素组内列序相反 */
                const uint8_t cw_e = (rg & 1U) ? (uint8_t)(7U - cw) : cw;
                line               = (uint8_t)(y % 4U);
                step               = (uint16_t)(c * 8U + cw_e);
                ch                 = 0U;
            } else {
                const uint8_t y_in = (uint8_t)(y - CH0_ROWS); /* 0..7 */
                const uint8_t rg   = (uint8_t)(y_in / 4U);    /* 0..1 */
                const uint8_t c    = (uint8_t)(block * 2U + rg);
                /* 同理：ch1 的奇数行组（20-23）组内列序相反 */
                const uint8_t cw_e = (rg & 1U) ? (uint8_t)(7U - cw) : cw;
                line               = (uint8_t)(y_in % 4U);
                /* ch1 只有 96 位，放在后 96 步，避免被 192 个时钟移出寄存器 */
                step               = (uint16_t)(96U + c * 8U + cw_e);
                ch                 = 1U;
            }

            const uint32_t idx = ((uint32_t)line * STEPS_PER_LINE + step) * MOD_CHANNELS + ch;
            dev->hub75_buff[idx] = dev->pixel_map[(uint32_t)y * MOD_COLS + x];
        }
    }
}

/* ================================================================
 *  scan: 按物理移位顺序输出一条扫描线
 *
 *  每个时钟同时给两条通道各写一次 RGB 引脚，然后打一个 CLK。
 *  ch1 在 step ≥ CH1_STEPS_PER_LINE 时输出 0（溢出位，不可见）。
 * ================================================================ */
/* ---- 灯序测试的历史轨迹：1 = 已点亮过（显示红色） ----
 * 由 `_scan` 在测试点变化时自动登记，退出测试模式（test_step < 0）时清空。
 * 只由扫描任务读写，测试任务只改 test_step/test_ch，竞争最坏是丢一次登记。 */
[[gnu::section(".ccmram")]] static uint8_t s_trail[STEPS_PER_LINE * MOD_CHANNELS];
static int16_t                     s_trail_step = -1;
static uint8_t                     s_trail_ch;

static void _scan(dev_display_t *dev, uint8_t line)
{
    const uint16_t base   = (uint16_t)((uint16_t)line * STEPS_PER_LINE * MOD_CHANNELS);
    const bool     test   = (dev->test_step >= 0);
    const uint16_t t_step = (uint16_t)dev->test_step;

    if (test) {
        /* 测试点变了 → 把上一个点记进轨迹（下一帧起显示红色） */
        if (s_trail_step >= 0 &&
            (s_trail_step != dev->test_step || s_trail_ch != dev->test_ch)) {
            s_trail[(uint16_t)s_trail_step * MOD_CHANNELS + s_trail_ch] = 1U;
        }
        s_trail_step = dev->test_step;
        s_trail_ch   = dev->test_ch;
    } else if (s_trail_step >= 0) {
        /* 退出测试模式：清轨迹，并复位状态 */
        memset(s_trail, 0, sizeof(s_trail));
        s_trail_step = -1;
        s_trail_ch   = 0;
    }

    for (uint16_t step = 0; step < STEPS_PER_LINE; step++) {
        for (uint8_t ch = 0; ch < MOD_CHANNELS; ch++) {
            uint8_t color;
            if (test) {
                const uint32_t ti = (uint32_t)step * MOD_CHANNELS + ch;
                if (step == t_step && ch == dev->test_ch)
                    color = DEV_DISPLAY_COLOR_GREEN;           /* 当前点 */
                else if (s_trail[ti])
                    color = DEV_DISPLAY_COLOR_RED;             /* 亮过的点 */
                else
                    color = DEV_DISPLAY_COLOR_BLACK;
            } else {
                color = dev->hub75_buff[base + (uint16_t)step * MOD_CHANNELS + ch];
            }
            const hub75_bsrr_t *b = &s_bsrr[ch][color];
            pl_hub75_bsrr_flush(&b->r);
            pl_hub75_bsrr_flush(&b->g);
            pl_hub75_bsrr_flush(&b->b);
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: 1/4 扫，只有 A/B 两根地址线参与（C/D 由 pl_hub75_set_row 一并写 0） ---- */
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

    /* 开机横幅：没有分析仪时，用它确认"注册的是哪个模组、参数是多少" */
    printf("[disp] p10_48x24 %ux%u scan=%u ch=%u steps=%u buff=%u\n",
           (unsigned)MOD_COLS, (unsigned)MOD_ROWS, (unsigned)MOD_SCAN_LINES,
           (unsigned)MOD_CHANNELS, (unsigned)STEPS_PER_LINE, (unsigned)BUFF_SIZE);

    for (uint8_t ch = 0; ch < MOD_CHANNELS; ch++) {
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
