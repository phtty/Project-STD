/**
 * @file    dev_display_p10.c
 * @brief   P10 模组派生类型 — 1/2 扫描、50 数据通道（自 B 工程 display.c 移植）
 *
 * 实现 dev_display_ops:
 *   prepare: convert_pixelmap (像素重排) + prepare_send_buffer (预计算 BSRR 整行表)
 *   scan:    整行 336 步 × 7 端口 BSRR 推送 + CLK 脉冲（OE/LAT 原子窗口由框架负责）
 *   set_row: 1/2 扫描行地址编码（注意 1-based, 框架传入 0-based, 需 +1）
 *
 * 屏幕 224×50, 2 行 × 5 列 P10 模组, 每模组 5 通道, 颜色不走固定 R/G/B 通道,
 * 由 convert_pixelmap 的组映射与 prepare_send_buffer 的逐通道位拆解共同决定。
 */

#include "dev_display.h"

#include <string.h>
#include "main.h"
#include "initcall.h"

/* ================================================================
 *  P10 模组参数（自 B 工程 display.h）
 *
 *  直通格栅模组, 无法级联: 每个模组独占 1 个 HUB75 接口。
 *  模组分左/右两种, 一左一右构成一排 (2 模组拼出屏宽 224),
 *  5 排堆叠构成全屏 (屏高 50)。共 10 模组 = 10 接口 × 5 通道 = 50 数据通道。
 * ================================================================ */

#define MODULES_PER_ROW     (2U)   /* 每排模组数: 左模组 + 右模组 */
#define MODULES_PER_COL     (5U)   /* 每列模组数: 5 排堆叠 */
#define MODULE_PIXEL_ROW    (112U) /* 单模块长轴像素数 (屏宽方向, 左/右各占一半) */
#define MODULE_PIXEL_COL    (10U)  /* 单模块短轴像素数 (屏高方向) */
#define CHANNELS_PER_MODULE (5U)   /* 每模块通道数 (接口 6 数据线启用 5) */
#define SCAN_LINES          (2U)   /* 1/2 扫描 */

#define GROUP_SIZE          (MODULE_PIXEL_ROW * SCAN_LINES) /* 每模组显存块 224 字节 */

#define SCREEN_ROWS         (MODULES_PER_ROW * MODULE_PIXEL_ROW) /* 屏宽 224 = 左112 + 右112 */
#define SCREEN_COLS         (MODULES_PER_COL * MODULE_PIXEL_COL) /* 屏高 50 = 5 排 × 10 */
#define BUFFER_SIZE         (SCREEN_ROWS * SCREEN_COLS)          /* 11200 */
/* 非级联直通屏: 每模组独占接口, 通道数 = 模组总数 × 每模组通道数
   (级联屏公式 modules_per_col × channels_per_module 不适用) */
#define TOTAL_CHANNELS   (MODULES_PER_ROW * MODULES_PER_COL * CHANNELS_PER_MODULE) /* 2×5×5 = 50 */
#define CHANNEL_PIXELS   (MODULE_PIXEL_ROW * SCAN_LINES)                           /* 每通道像素 224 = 112 × 2 扫行 */
#define SCAN_LINE_PIXELS (CHANNEL_PIXELS / SCAN_LINES)                             /* 112 */
#define TIMING_STEPS     (SCAN_LINE_PIXELS * 3)                                    /* 336: 每像素 R/G/B 各一步 */
#define USED_PORT_COUNT  (7U)                                                      /* PA..PG */

/* 原先这里有一条与 dev_display.h 的 DEV_DISPLAY_SCREEN_ROWS/COLS 的交叉校验。
   那两个宏已从共享头移除：一块板上可以接多种模组，共享头里放一个编译期几何
   不成立。几何改为运行期从 dev_display_t 读（本文件第 140~151 行从上面这些宏
   填入，宏仍是唯一真源），跨模块的一致性由 _render_init 的运行期校验兜底。 */

/* ---- 端口索引（与 B 工程 port_index 一致） ---- */
enum {
    PA_IDX = 0,
    PB_IDX,
    PC_IDX,
    PD_IDX,
    PE_IDX,
    PF_IDX,
    PG_IDX,
};

typedef struct {
    int8_t port_idx;
    uint16_t pin;
} channel_info_t;

/* ---- 通道映射（B 工程 display.c 生效版本, 升序, 50 通道） ---- */
static const channel_info_t channel_map[TOTAL_CHANNELS] = {
    {PA_IDX, LED_CH0_Pin},
    {PA_IDX, LED_CH1_Pin},
    {PA_IDX, LED_CH2_Pin},
    {PC_IDX, LED_CH3_Pin},
    {PC_IDX, LED_CH4_Pin},
    {PF_IDX, LED_CH6_Pin},
    {PF_IDX, LED_CH7_Pin},
    {PF_IDX, LED_CH8_Pin},
    {PF_IDX, LED_CH9_Pin},
    {PF_IDX, LED_CH10_Pin},
    {PF_IDX, LED_CH12_Pin},
    {PF_IDX, LED_CH13_Pin},
    {PF_IDX, LED_CH14_Pin},
    {PC_IDX, LED_CH15_Pin},
    {PC_IDX, LED_CH16_Pin},
    {PE_IDX, LED_CH18_Pin},
    {PE_IDX, LED_CH19_Pin},
    {PE_IDX, LED_CH20_Pin},
    {PE_IDX, LED_CH21_Pin},
    {PE_IDX, LED_CH22_Pin},
    {PD_IDX, LED_CH24_Pin},
    {PD_IDX, LED_CH25_Pin},
    {PD_IDX, LED_CH26_Pin},
    {PD_IDX, LED_CH27_Pin},
    {PD_IDX, LED_CH28_Pin},
    {PD_IDX, LED_CH30_Pin},
    {PD_IDX, LED_CH31_Pin},
    {PC_IDX, LED_CH32_Pin},
    {PA_IDX, LED_CH33_Pin},
    {PA_IDX, LED_CH34_Pin},
    {PC_IDX, LED_CH36_Pin},
    {PG_IDX, LED_CH37_Pin},
    {PG_IDX, LED_CH38_Pin},
    {PG_IDX, LED_CH39_Pin},
    {PG_IDX, LED_CH40_Pin},
    {PG_IDX, LED_CH42_Pin},
    {PD_IDX, LED_CH43_Pin},
    {PD_IDX, LED_CH44_Pin},
    {PD_IDX, LED_CH45_Pin},
    {PD_IDX, LED_CH46_Pin},
    {PB_IDX, LED_CH48_Pin},
    {PB_IDX, LED_CH49_Pin},
    {PB_IDX, LED_CH50_Pin},
    {PB_IDX, LED_CH51_Pin},
    {PB_IDX, LED_CH52_Pin},
    {PE_IDX, LED_CH54_Pin},
    {PE_IDX, LED_CH55_Pin},
    {PE_IDX, LED_CH56_Pin},
    {PE_IDX, LED_CH57_Pin},
    {PE_IDX, LED_CH58_Pin},
};

/* ---- P10 实例 ---- */
typedef struct {
    dev_display_t base;
} dev_module_t;

[[gnu::section(".ccmram")]] static uint8_t pixel_map[BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t hub75_buff[BUFFER_SIZE];
/* BSRR 预计算表: [扫行][时序步][端口]
 *
 * **不做双缓冲**（B 原版是 [2][...] 双 bank + IO_Flag 翻转）。本框架下
 * prepare 与 scan 由同一个 scan_task 顺序调用（dev_display.c：dirty 时先
 * prepare，紧接着 scan 当前行），不存在"一边填一边读"的并发，第二个 bank
 * 纯属从 B 原架构带过来的遗留。而它值 18816 字节 CCMRAM —— 双 bank 时
 * 本板 CCMRAM 超出 6988 字节根本链接不了。
 * 若日后 prepare 与 scan 被拆到不同上下文，必须把双缓冲加回来。 */
[[gnu::section(".ccmram")]] static uint32_t hub75_IO[SCAN_LINES][TIMING_STEPS][USED_PORT_COUNT];

/* 直通格栅屏实例: 一左一右两模组构成一排 (modules_per_row=2),
   5 排堆叠 (modules_per_col=5), 每模组独占 1 接口 (total_channels=10×5)。
   注意 total_channels / channel_pixels 不满足 dev_display.h 注释中的级联屏公式,
   此处按直通屏物理结构直接赋值。 */
static dev_module_t s_module = {
    .base = {
        .ops                 = nullptr,             /* 由 dev_display_init 设置 */
        .module_rows         = MODULE_PIXEL_ROW,    /* 112: 模组长轴 (屏宽方向) */
        .module_cols         = MODULE_PIXEL_COL,    /* 10: 模组短轴 (屏高方向) */
        .channels_per_module = CHANNELS_PER_MODULE, /* 5 */
        .modules_per_row     = MODULES_PER_ROW,     /* 2: 左模组 + 右模组 */
        .modules_per_col     = MODULES_PER_COL,     /* 5: 排数 */
        .scan_lines          = SCAN_LINES,          /* 2: 1/2 扫描 */
        .screen_rows         = SCREEN_ROWS,         /* 224: 屏宽 */
        .screen_cols         = SCREEN_COLS,         /* 50: 屏高 */
        .total_channels      = TOTAL_CHANNELS,      /* 50: 模组总数 × 每模组通道数 */
        .channel_pixels      = CHANNEL_PIXELS,      /* 224: 每通道像素 (含 2 扫行) */
        .scan_line_pixels    = SCAN_LINE_PIXELS,    /* 112 */
        .buffer_size         = BUFFER_SIZE,         /* 11200 */
        .pixel_map           = pixel_map,
        .hub75_buff          = hub75_buff,
        .light_level         = 7,
    },
};

/* ================================================================
 *  convert_pixelmap: pixel_map → hub75_buff 像素重排（B 工程逐字移植）
 *
 *  pixel_map[] 行优先 (y * screen_rows + x), x=0..223, y=0..49。
 *  屏幕左右两半（col < 112 为上排模组, 其余为下排）映射到不同模组组号,
 *  组号按行对 (y%2) 区分上下扫描行。
 * ================================================================ */

static void convert_pixelmap(dev_display_t *dev)
{
    uint16_t moduel_group = 0;
    uint16_t row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < dev->buffer_size; map_cnt++) {
        /* 大缓冲区行列分离 */
        row_cnt = map_cnt / dev->screen_rows; /* y: 0..49 */
        col_cnt = map_cnt % dev->screen_rows; /* x: 0..223 */

        if (col_cnt < MODULE_PIXEL_ROW) { /* 屏幕左侧模块映射 */
            /* 大缓冲区组号 */
            moduel_group = row_cnt / 10 * 10 + row_cnt % 10 / 2 + 5;
            /* 大缓冲区映射到显存 */
            dev->hub75_buff[col_cnt + row_cnt % 2 * MODULE_PIXEL_ROW + moduel_group * GROUP_SIZE] = dev->pixel_map[map_cnt];
        } else { /* 屏幕右侧模块映射, 同时模组内的通道也要倒序 */
            /* 大缓冲区组号 */
            moduel_group = row_cnt / 10 * 10 + 4 - row_cnt % 10 / 2;
            /* 大缓冲区映射到显存 */
            dev->hub75_buff[(MODULE_PIXEL_ROW * 2 - col_cnt - 1) + row_cnt % 2 * MODULE_PIXEL_ROW + moduel_group * GROUP_SIZE] = dev->pixel_map[map_cnt];
        }
    }
}

/* ================================================================
 *  prepare_send_buffer: 显存 → 双缓冲 BSRR 整行表（B 工程逐字移植）
 *
 *  每个时序步对应一个像素的一种颜色 (step%3 → RGB), 颜色位从显存像素值
 *  (bit0=R, bit1=G, bit2=B) 拆解, 逐通道按 channel_map 的端口/引脚累加
 *  到该端口该步的 BSRR 字 (置位 = 亮, 复位 = 灭)。
 * ================================================================ */

static void prepare_send_buffer(dev_display_t *dev)
{
    /* 清空发送缓冲 */
    memset(hub75_IO, 0, sizeof(hub75_IO));

    /* 按扫描行 */
    for (uint8_t scan_idx = 0; scan_idx < SCAN_LINES; scan_idx++) {
        /* 该扫描行要发送的时序步数 */
        for (uint16_t step_cnt = 0; step_cnt < TIMING_STEPS; step_cnt++) {
            uint32_t pixel_offset   = step_cnt / 3 + scan_idx * SCAN_LINE_PIXELS;
            uint8_t color_bit_shift = 2 - (step_cnt % 3);

            /* 计算所有通道 */
            for (uint8_t ch = 0; ch < TOTAL_CHANNELS; ch++) {
                /* 取显存数据 */
                uint32_t ram_idx = (ch * CHANNEL_PIXELS) + pixel_offset;

                /* 取出该通道对应的port以及pin在寄存器中的bit位 */
                uint8_t port_idx = channel_map[ch].port_idx;
                uint16_t pin     = channel_map[ch].pin;

                /* 将对应BSRR寄存器状态写入发送缓冲 */
                if (dev->hub75_buff[ram_idx] & (1 << color_bit_shift)) {
                    hub75_IO[scan_idx][step_cnt][port_idx] |= pin;
                } else {
                    hub75_IO[scan_idx][step_cnt][port_idx] |= ((uint32_t)pin << 16);
                }
            }
        }
    }

}

/* ---- prepare: 像素重排 + 预计算（B 工程 convert_pixelmap 尾部即调用） ---- */
static void _prepare(dev_display_t *dev)
{
    convert_pixelmap(dev);
    prepare_send_buffer(dev);
}

/* ================================================================
 *  scan: 整行输出 — 336 步 × 7 端口 BSRR 推送 + CLK 脉冲（B 工程逐字移植）
 *
 *  OE/LAT 行切换由 dev_display 框架的原子窗口统一处理。
 * ================================================================ */

static void _scan(dev_display_t *dev, uint8_t line)
{
    (void)dev;
    uint32_t *pData = &hub75_IO[line][0][0];

    /* 逐行发送该扫描行每个像素的每种颜色 */
    for (uint16_t line_cnt = 0; line_cnt < TIMING_STEPS; line_cnt++) {
        for (uint8_t i = 0; i < USED_PORT_COUNT; i++) {
            if (*pData)
                pl_hub75_port_by_idx(i)->BSRR = *pData;

            pData++;
        }

        /* CLK 一个脉冲, LED 驱动芯片移位寄存器移位 */
        pl_hub75_clock_pulse();
    }
}

/* ---- set_row: 1/2 扫描行地址（框架传入 0-based, B 工程为 1-based, 需 +1） ---- */
static void _set_row(uint8_t row)
{
    pl_hub75_set_row(row + 1);
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t ops = {
    .prepare = _prepare,
    .scan    = _scan,
    .set_row = _set_row,
};

void dev_module_init(void)
{
    s_module.base.ops = &ops;
    dev_display_register(&s_module.base);
}
hw_dev_initcall(dev_module_init);
