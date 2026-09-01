/**
 * @file    dev_display_1_263.c
 * @brief   1-263 模组派生类型 — P6 户外全彩（32x32 单模块，1/8 扫描）
 *
 * 实现 dev_display_ops: prepare (pixel_map→hub75_buff) + scan (BSRR 查表输出)
 * g_bsrr / row_dst 查表放在 CCMRAM 中，避免 Flash 读取和分支。
 *
 * 仅改下方模组宏即可驱动不同拼屏尺寸；映射规则为 1-263 单模块 32 列直排。
 *
 * 历史教训（旧版曾把 PIXEL_ROW 写成 64 才"能显示"的原因）：
 *   旧版照抄 1-577 的 64 宽左右半区布局 → 每扫描线发 128 时钟。本模组
 *   移位链只有 64 bit：前 64 时钟的数据被移出链尾丢弃，模组实际保留
 *   后 64 bit = 64 宽虚拟画布的右半（X=32..63）→ 整屏只显示右半幅，
 *   但行结构正确、字形完整，误判为"正常"。PIXEL_ROW 改 32 时又沿用
 *   1-577 的左右半区 prepare（半宽 16），只写入 16+16 列 → 显示残缺。
 *   正确写法即本文件：32 列整行直排、64 时钟。
 *   若整屏由 2 张 32 列模组级联（64 宽），把 MODULE_ROWS 改为 2 即可
 *   （SCAN_LINE_PX 自动推导为 128，输出位流与旧版 64 布局完全一致）。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"

/* ================================================================
 *  1-263 模组参数 — P6 户外全彩
 *
 *  单模块 32x32 像素，HUB75 2 通道/模块，ABC 1/8 扫描。
 *  行驱动 16206S（译码器型，A/B/C 行址直通，兼容 1-577 行选时序），
 *  列驱动 2013EP（16 通道恒流，每时钟移入 1 bit/通道，可级联）。
 * ================================================================ */
#define MODULE_CODE                "1000000263"

#define _1_263_MODULE_ROWS         (1U)  /* 每行模块数（水平） */
#define _1_263_MODULE_COLS         (1U)  /* 每列模块数（垂直） */
#define _1_263_MODULE_PIXEL_ROW    (32U) /* 单模块每行的像素个数 */
#define _1_263_MODULE_PIXEL_COL    (32U) /* 单模块每列的像素个数 */
#define _1_263_CHANNELS_PER_MODULE (2U)  /* 每模块通道数（R1G1B1 + R2G2B2） */
#define _1_263_SCAN_LINES          (8U)  /* 1/8 扫描 */

/* ---- 派生参数（由模组参数计算，勿手动修改） ---- */
#define _1_263_SCREEN_ROWS    (_1_263_MODULE_ROWS * _1_263_MODULE_PIXEL_ROW) /* 屏幕每行像素数 */
#define _1_263_SCREEN_COLS    (_1_263_MODULE_COLS * _1_263_MODULE_PIXEL_COL) /* 屏幕每列像素数 */
#define _1_263_BUFFER_SIZE    (_1_263_SCREEN_ROWS * _1_263_SCREEN_COLS)
#define _1_263_TOTAL_CHANNELS (_1_263_MODULE_COLS * _1_263_CHANNELS_PER_MODULE)
#define _1_263_CHANNEL_PIXELS (_1_263_MODULE_PIXEL_ROW * _1_263_MODULE_PIXEL_COL * _1_263_MODULE_ROWS / _1_263_CHANNELS_PER_MODULE)
#define _1_263_SCAN_LINE_PX   (_1_263_CHANNEL_PIXELS / _1_263_SCAN_LINES)

/* ================================================================
 *  hub75_buff 布局（1-263 单模块直排）
 *
 *  每通道内按 scan line 连续存放；每条 scan line 内按模块横向拼接：
 *    单模块 64 字节 = 高行 32 + 低行 32
 *      [0..31]   = Y_high, X=0..31
 *      [32..63]  = Y_low,  X=0..31
 *    后续模块各占 MODULE_BUFF_STRIDE 字节。
 *
 *  移位链长度推导（每扫描线每通道 64 bit = 64 时钟）：
 *    1/8 扫 → 每扫描线点亮 4 物理行；2 通道各管 2 行 →
 *    每通道 2 行 × 32 列 = 64 输出；2013EP 16 通道 → 4 颗串联/数据线。
 *    6 数据线 × 64 时钟 = 384 bit = 4 行 × 32 列 × RGB，逐位对应无空位。
 *
 *  行映射（1/8 扫描，每通道覆盖 ROWS_PER_CHANNEL 行）：
 *    local_y < SCAN_LINES → Y_low  （写入半区偏移 PIXEL_ROW）
 *    local_y >= SCAN_LINES → Y_high（写入半区偏移 0）
 *    ch0: Y_low=行 L(0..7), Y_high=行 L+8；ch1: 行 L+16 / L+24
 * ================================================================ */

#define _1_263_MODULE_BUFF_STRIDE (_1_263_MODULE_PIXEL_ROW * 2U)              /* 64：单模块 scan 占用 */
#define _1_263_ROWS_PER_CHANNEL   (_1_263_MODULE_PIXEL_COL / _1_263_CHANNELS_PER_MODULE) /* 16 */

/* ---- BSRR 预计算查表 ---- */
typedef struct {
    pl_hub75_bsrr_t r, g, b;
} _1_263_bsrr_t;

[[gnu::section(".ccmram")]] static _1_263_bsrr_t g_bsrr[_1_263_TOTAL_CHANNELS][8];

/* ---- 1-263 实例 ---- */
typedef struct {
    dev_display_t me;
} dev_display_1_263_t;

[[gnu::section(".ccmram")]] static uint8_t _1_263_pixel_map[_1_263_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t _1_263_hub75_buff[_1_263_BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint16_t _1_263_row_dst[_1_263_SCREEN_COLS];

static dev_display_1_263_t g_1_263 = {
    .me = {
        .ops                 = nullptr, /* 由 dev_display_1_263_init 设置 */
        .module_rows         = _1_263_MODULE_PIXEL_ROW,
        .module_cols         = _1_263_MODULE_PIXEL_COL,
        .channels_per_module = _1_263_CHANNELS_PER_MODULE,
        .modules_per_row     = _1_263_MODULE_ROWS,
        .modules_per_col     = _1_263_MODULE_COLS,
        .scan_lines          = _1_263_SCAN_LINES,
        .screen_rows         = _1_263_SCREEN_ROWS,
        .screen_cols         = _1_263_SCREEN_COLS,
        .total_channels      = _1_263_TOTAL_CHANNELS,
        .channel_pixels      = _1_263_CHANNEL_PIXELS,
        .scan_line_pixels    = _1_263_SCAN_LINE_PX,
        .buffer_size         = _1_263_BUFFER_SIZE,
        .pixel_map           = _1_263_pixel_map,
        .hub75_buff          = _1_263_hub75_buff,
        .module_code         = MODULE_CODE,
        .light_level         = DEV_DISPLAY_BRIGHTNESS_MAX,
    },
};

dev_display_t *dev_display_1_263_get(void)
{
    return &g_1_263.me;
}

/* ================================================================
 *  prepare: pixel_map → hub75_buff 像素重排
 *
 *  pixel_map[] 按行优先存储（y * screen_rows + x），
 *  hub75_buff[] 按 1-263 物理移位顺序组织（每模块 32 列整行直排）。
 *  行起始偏移由 init 预计算到 _1_263_row_dst[]，此处只做拷贝。
 * ================================================================ */

static void _1_263_prepare(dev_display_t *dev)
{
    const uint16_t screen_rows = dev->screen_rows;
    const uint16_t screen_cols = dev->screen_cols;
    const uint8_t *pixel_map   = dev->pixel_map;
    uint8_t *hub75_buff        = dev->hub75_buff;

    for (uint16_t row = 0; row < screen_cols; row++) {
        const uint8_t *src = pixel_map + (uint16_t)(row * screen_rows);
        uint8_t *dst_base  = hub75_buff + _1_263_row_dst[row];

        for (uint16_t m = 0; m < _1_263_MODULE_ROWS; m++) {
            /* 单模块 32 列整行直排：落入 row_dst 指定的高/低行组半区 */
            memcpy(dst_base + m * _1_263_MODULE_BUFF_STRIDE,
                   src + m * _1_263_MODULE_PIXEL_ROW,
                   _1_263_MODULE_PIXEL_ROW);
        }
    }
}

/* ================================================================
 *  scan: 逐像素输出 — BSRR 查表 + CLK 脉冲
 *
 *  每扫描线 64 时钟（每通道 64 bit 链长），每时钟 ch0/ch1 各取 1 字节
 *  颜色索引，查 g_bsrr 输出 R/G/B，时钟上升沿移入模组移位链。
 * ================================================================ */

static inline void _1_263_scan(dev_display_t *dev, uint8_t line)
{
    const uint16_t scan_line_pixels = dev->scan_line_pixels;
    const uint16_t channel_pixels   = dev->channel_pixels;
    const uint8_t total_channels    = dev->total_channels;
    const _1_263_bsrr_t (*bsrr)[8]  = g_bsrr;
    const uint8_t *base             = dev->hub75_buff + (uint16_t)line * scan_line_pixels;

    for (uint16_t pixel = 0; pixel < scan_line_pixels; pixel++, base++) {
        const uint8_t *channel_ptr = base;
        for (uint8_t ch = 0; ch < total_channels; ch++) {
            uint8_t color = channel_ptr[(uint16_t)channel_pixels * ch];
            const _1_263_bsrr_t *entry = &bsrr[ch][color];
            pl_hub75_bsrr_flush(&entry->r);
            pl_hub75_bsrr_flush(&entry->g);
            pl_hub75_bsrr_flush(&entry->b);
        }
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: 16206S 译码器型行驱动（A/B/C 直通二进制行址，D 恒 0） ---- */
static void _1_263_set_row(uint8_t row)
{
    pl_hub75_Decoder_set_row(row);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t _1_263_ops = {
    .prepare = _1_263_prepare,
    .scan    = _1_263_scan,
    .set_row = _1_263_set_row,
};

/* ================================================================
 *  dev_display_1_263_init: 预计算 row_dst / BSRR 查表 + 绑定 ops
 *
 *  _1_263_row_dst[y] = 该逻辑行在 hub75_buff 中的起始偏移：
 *    ch * CHANNEL_PIXELS + line * SCAN_LINE_PX + row_type * PIXEL_ROW
 *  其中 row_type: 1=low（偏移 PIXEL_ROW=32），0=high（偏移 0）。
 *  g_bsrr[ch][c] 存储通道 ch 在颜色 c (0~7) 时的 R/G/B 引脚输出值。
 * ================================================================ */

void dev_display_1_263_init(void)
{
    g_1_263.me.ops = &_1_263_ops;
    dev_display_register(&g_1_263.me);

    for (uint16_t row = 0; row < _1_263_SCREEN_COLS; row++) {
        const uint16_t ch       = (uint16_t)(row / _1_263_ROWS_PER_CHANNEL);
        const uint16_t local_y  = (uint16_t)(row % _1_263_ROWS_PER_CHANNEL);
        const uint16_t line     = (uint16_t)(local_y % _1_263_SCAN_LINES);
        const uint16_t row_type = (local_y < _1_263_SCAN_LINES) ? 1U : 0U; /* 1=low, 0=high */
        _1_263_row_dst[row] =
            (uint16_t)(ch * _1_263_CHANNEL_PIXELS + line * _1_263_SCAN_LINE_PX +
                       row_type * _1_263_MODULE_PIXEL_ROW);
    }

    for (uint8_t ch = 0; ch < g_1_263.me.total_channels; ch++) {
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
hw_dev_initcall(dev_display_1_263_init);