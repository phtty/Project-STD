#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  模组参数
 * ================================================================ */

#define MODULE_ROWS         (10U) /* 每行模块数 */
#define MODULE_COLS         (4U)  /* 每列模块数 */
#define MODULE_PIXEL_ROW    (32U) /* 单模块像素行数 */
#define MODULE_PIXEL_COL    (16U) /* 单模块像素列数 */
#define CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define SCAN_LINES          (4U)  /* 扫描行个数 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define SCREEN_ROWS    (MODULE_ROWS * MODULE_PIXEL_ROW)                                          // 整屏每行的像素个数
#define SCREEN_COLS    (MODULE_COLS * MODULE_PIXEL_COL)                                          // 整屏每列的像素个数
#define BUFFER_SIZE    (SCREEN_ROWS * SCREEN_COLS)                                               // 整屏使用的缓冲区大小（Byte）
#define TOTAL_CHANNELS (MODULE_COLS * CHANNELS_PER_MODULE)                                       // 整屏通道总数
#define CHANNEL_PIXELS (MODULE_PIXEL_ROW * MODULE_PIXEL_COL * MODULE_ROWS / CHANNELS_PER_MODULE) // 单个通道像素个数
#define SCAN_LINE_PX   (CHANNEL_PIXELS / SCAN_LINES)                                             // 单个扫描行像素个数

// 组结构定义
#define GROUP_SIZE    (16U)             /* 每组的像素个数 */
#define GROUP_PER_ROW (MODULE_ROWS * 4) /* 每个组行的组个数 */

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} module_bsrr_t;

[[gnu::section(".ccmram")]] static module_bsrr_t gs_bsrr[TOTAL_CHANNELS][8];

/* ---- 定义当前模组类 ---- */
typedef struct {
    dev_display_t me; // 常规模组，可用父类描述所有功能
} dev_display_module_t;

[[gnu::section(".ccmram")]] static uint8_t pixel_map[BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t hub75_buff[BUFFER_SIZE];

static dev_display_module_t module = {
    .me = {
        .ops                 = nullptr, /* 由模块初始化函数设置 */
        .module_rows         = MODULE_PIXEL_ROW,
        .module_cols         = MODULE_PIXEL_COL,
        .channels_per_module = CHANNELS_PER_MODULE,
        .modules_per_row     = MODULE_ROWS,
        .modules_per_col     = MODULE_COLS,
        .scan_lines          = SCAN_LINES,
        .screen_rows         = SCREEN_ROWS,
        .screen_cols         = SCREEN_COLS,
        .total_channels      = TOTAL_CHANNELS,
        .channel_pixels      = CHANNEL_PIXELS,
        .scan_line_pixels    = SCAN_LINE_PX,
        .buffer_size         = BUFFER_SIZE,
        .pixel_map           = pixel_map,
        .hub75_buff          = hub75_buff,
        .light_level         = 7,
    },
};

/* ================================================================
 *  prepare: pixel_map → hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按"组"组织以匹配 HUB75 移位寄存器输入时序。
 *  此函数将像素值从逻辑坐标映射到硬件输出缓冲。
 * ================================================================ */

static void _prepare(dev_display_t *dev)
{
    int32_t group_index = 0; /* 当前像素所属的"组"索引 */
    int32_t group_col   = 0; /* 当前像素所属的"组列" */
    int32_t group_row   = 0; /* 当前像素所属的"组行" */
    int32_t pixel_col   = 0; /* 像素列坐标x */
    int32_t pixel_row   = 0; /* 像素行坐标y */

    for (int32_t linear = 0; linear < (int32_t)dev->buffer_size; linear++) {
        pixel_col = linear % (int32_t)dev->screen_rows; /* 行优先: x 先递增 */
        pixel_row = linear / (int32_t)dev->screen_rows;

        /* 计算组索引: 每组 4×4 像素，横向 GROUP_PIXEL_W 列、纵向 GROUP_PIXEL_H 行 */
        group_col   = pixel_col / 8;
        group_row   = pixel_row / 8 * 4 + pixel_row % 4;
        group_index = group_row * GROUP_PER_ROW + group_col;

        /* 组内映射 */
        dev->hub75_buff[pixel_row / 4 % 2 * 8 + pixel_col % 8 + group_index * GROUP_SIZE] = dev->pixel_map[linear];
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  hub75_buff[] 存储的是颜色索引 (0~7)，扫描时查 gs_bsrr
 *  得到该通道、该颜色的 R/G/B 三组 {port, BSRR_val}，直接 flush 输出。
 * ================================================================ */

static inline void _scan(dev_display_t *dev, uint8_t line)
{
    for (uint16_t pixel = 0; pixel < dev->scan_line_pixels; pixel++) {
        /* 当前像素在所有通道中的起始偏移 */
        uint16_t pixel_base = (uint16_t)line * dev->scan_line_pixels + pixel;

        /* 同一像素位置同时输出所有通道的颜色数据 */
        for (uint8_t ch = 0; ch < dev->total_channels; ch++) {
            uint8_t color       = dev->hub75_buff[pixel_base + ch * dev->channel_pixels];
            module_bsrr_t *bsrr = &gs_bsrr[ch][color];
            pl_hub75_bsrr_flush(&bsrr->r);
            pl_hub75_bsrr_flush(&bsrr->g);
            pl_hub75_bsrr_flush(&bsrr->b);
        }

        /* 锁存当前像素数据到移位寄存器 */
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: ABCD 行地址编码 ---- */
static void _set_row(uint8_t row)
{
    // 8421 BCB译码行地址
    pl_hub75_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t module_ops = {
    .prepare = _prepare,
    .scan    = _scan,
    .set_row = _set_row,
};

/* ================================================================
 *  预计算 BSRR 查表 + 绑定 ops
 *
 *  gs_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 *  颜色 c 的 bit0→R, bit1→G, bit2→B:
 *    - R 亮: (c & 1) != 0 → BSRR 置位（高电平）
 *    - R 灭: (c & 1) == 0 → BSRR 复位（低电平）
 *    - G 亮: (c & 2) != 0,  B 亮: (c & 4) != 0
 *
 *  预计算避免扫描热路径中的分支判断。
 * ================================================================ */
void dev_p10_32x16_22200001703_init(void)
{
    module.me.ops = &module_ops;
    dev_display_register(&module.me);

    for (uint8_t ch = 0; ch < module.me.total_channels; ch++) {
        for (uint8_t color = 0; color < 8; color++) {
            /* R 通道: color bit0 决定亮灭 */
            gs_bsrr[ch][color].r.port = g_hub75_pin_r[ch].port;
            gs_bsrr[ch][color].r.val  = (color & 1) ? (uint32_t)g_hub75_pin_r[ch].pin        /* 置位: 输出高 */
                                                    : (uint32_t)g_hub75_pin_r[ch].pin << 16; /* 复位: 输出低 */

            /* G 通道: color bit1 决定亮灭 */
            gs_bsrr[ch][color].g.port = g_hub75_pin_g[ch].port;
            gs_bsrr[ch][color].g.val  = (color & 2) ? (uint32_t)g_hub75_pin_g[ch].pin
                                                    : (uint32_t)g_hub75_pin_g[ch].pin << 16;

            /* B 通道: color bit2 决定亮灭 */
            gs_bsrr[ch][color].b.port = g_hub75_pin_b[ch].port;
            gs_bsrr[ch][color].b.val  = (color & 4) ? (uint32_t)g_hub75_pin_b[ch].pin
                                                    : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }
}
hw_dev_initcall(dev_p10_32x16_22200001703_init);
