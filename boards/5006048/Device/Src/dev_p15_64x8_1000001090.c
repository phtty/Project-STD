/**
 * @file    dev_p15_64x8_1000001090.c
 * @brief   P15 64×8 模组派生类型 — 静态扫描、每模组 4 路串行数据
 *
 * 实现 dev_display_ops:
 *   prepare: _convert_pixelmap（像素 → 通道优先的颜色缓冲）
 *            + _prepare_send_buffer（预计算 [时序步][端口] 的 BSRR 整帧表）
 *   scan:    384 步 × 7 端口 BSRR 推送 + CLK 脉冲（OE/LAT 原子窗口由框架负责）
 *   set_row: 静态扫描没有行地址复用，A/B/C/D 恒拉低
 *
 * 屏幕 128×32 = 2 列 × 4 排 P15 64×8 模组，静态扫描（无行复用）。
 * 复刻对象是 P15_flip_BarBoard_BB/USER/TIMER/timer.c 的 lamp_scan()，但那条实现
 * 是没改完的旧代码（按 20 路/半屏扫、越界读），这里的结构、表与 32 路收口按
 * 本工程 P10 驱动的标准重写。**时序语义按老固件 1:1 保留**，理由见文件末尾
 * 「移植说明」。
 *
 * 本驱动与 P10 112×10 驱动是同一块控制卡的两种面板，二选一编译（每个驱动自带
 * 一份 CCMRAM 帧缓冲 + BSRR 表，多编一份直接顶爆 CCMRAM，且后注册者覆盖前者）。
 */

#include "dev_display.h"

#include "main.h"
#include "initcall.h"

/* ================================================================
 *  P15 模组参数
 *
 *  直通格栅模组，无法级联：每块模组独占控制卡上的一个 HUB75 接口。
 *  每块模组 64×8；每块 4 路串行数据，每路驱动 2 行（2 × 64 = 128 像素），
 *  每像素 R/G/B 各 1 bit 串行移入 → 每路 128 × 3 = 384 bit = 48 字节。
 *  4 路 × 2 行 = 8 行，正好一块模组。
 *
 *  屏体 128×32：左右各一列模组（各 64 列宽），上下 4 排（各 8 行高）。
 *  与老固件编译产物的符号尺寸对得上：sourceBuf = 128 × 32 = 4096、
 *  fixBuf = 32 通道 × 48 字节 = 1536。
 * ================================================================ */

#define MODULES_PER_ROW     (2U)  /**< 每排模组数：左列 + 右列 */
#define MODULES_PER_COL     (4U)  /**< 每列模组数：4 排堆叠 */
#define MODULE_PIXEL_ROW    (64U) /**< 单模组长轴像素数（屏宽方向） */
#define MODULE_PIXEL_COL    (8U)  /**< 单模组短轴像素数（屏高方向） */
#define CHANNELS_PER_MODULE (4U)  /**< 每模组 4 路串行数据（每路 2 行） */
#define ROWS_PER_CHANNEL    (2U)  /**< 每路数据驱动的像素行数 */
#define SCAN_LINES          (1U)  /**< 静态扫描：无行地址复用 */

#define SCREEN_ROWS         (MODULES_PER_ROW * MODULE_PIXEL_ROW) /**< 屏宽 128 */
#define SCREEN_COLS         (MODULES_PER_COL * MODULE_PIXEL_COL) /**< 屏高 32 */
#define BUFFER_SIZE         (SCREEN_ROWS * SCREEN_COLS)          /**< 4096 */

/* 非级联直通屏：每模组独占接口，通道数 = 模组总数 × 每模组通道数
   （dev_display.h 注释里的级联屏公式 modules_per_col × channels_per_module
   不适用于直通屏，P10 驱动同样是显式赋值）。 */
#define TOTAL_CHANNELS      (MODULES_PER_ROW * MODULES_PER_COL * CHANNELS_PER_MODULE) /**< 32 */
#define CHANNELS_PER_HALF   (TOTAL_CHANNELS / 2U)                                     /**< 16 */
#define CHANNEL_PIXELS      (ROWS_PER_CHANNEL * MODULE_PIXEL_ROW)                     /**< 128 */
#define SCAN_LINE_PIXELS    (CHANNEL_PIXELS / SCAN_LINES)                             /**< 128 */
#define POINTS_PER_CHANNEL  (CHANNEL_PIXELS * 3U)                                     /**< 384 个灯珠位 */
#define BYTES_PER_CHANNEL   (POINTS_PER_CHANNEL / 8U)                                 /**< 48 字节（老 fixBuf 的通道步长） */
#define TIMING_STEPS        (BYTES_PER_CHANNEL * 8U)                                  /**< 384：每像素 R/G/B 各一步 */
#define USED_PORT_COUNT     (7U)                                                      /**< PA..PG（PB 本屏不用，留位对齐） */

static_assert((TOTAL_CHANNELS * CHANNEL_PIXELS) == BUFFER_SIZE,
              "通道数 × 每通道像素数必须等于整屏像素数（hub75_buff 的布局前提）");
static_assert((POINTS_PER_CHANNEL % 8U) == 0U,
              "每通道灯珠位数必须整字节（老 fixBuf 按字节移位）");
static_assert((MODULE_PIXEL_ROW % 8U) == 0U,
              "模组宽必须是 8 的整数倍（一个字节覆盖 8 像素）");

/* ---- 端口索引（与 P10 驱动 / B 工程 port_index 一致） ---- */
enum {
    PA_IDX = 0,
    PB_IDX,
    PC_IDX,
    PD_IDX,
    PE_IDX,
    PF_IDX,
    PG_IDX,
};

/** @brief 单通道的端口与引脚 */
typedef struct {
    int8_t   port_idx; /**< 0..6 → GPIOA..GPIOG */
    uint16_t pin;      /**< GPIO_PIN_x */
} channel_info_t;

/* ---- 通道映射（32 路；下标 = hub75_buff 的通道序） ----
 *
 * 来源：老工程 USER/LAMP/lamp.h 的 LED_R1..R20/G/B 与本板 main.h 的
 * LED_CH0..CH59 是同一组网络名（R1=CH0=PA6、G1=CH1=PA4、B1=CH2=PA3、
 * R2=CH3=PC3 …… 逐条核对过），通道顺序取自老工程 USER/TIMER/timer.c 的
 * lamp_scan()。
 *
 * 控制卡的数据线按 5 个「12 线块」排列（CH0-11、CH12-23、CH24-35、CH36-47、
 * CH48-59），每块 = 两个 HUB75 接口 × 6 条颜色线（R1G1B1R2G2B2）。P15 64×8
 * 模组只用其中 4 条串行数据线：
 *     块内偏移 0..3 → 一块模组（左半屏 x 0..63）
 *     块内偏移 6..9 → 另一块模组（右半屏 x 64..127），且这 4 条线在块内**倒序**
 * 第 5 块（CH48-59）对应第 5 排模组：P15 屏只有 4 排（32 行），本屏不接。
 *
 * **老实现按 20 路/半屏（40 路）扫**，是没改完的旧代码：它把不存在的第 5 排也
 * 扫了 —— 左半屏那 4 路读到的是右半屏的数据（通道索引越界到下一半），右半屏
 * 那 4 路直接读到 fixBuf（1536B = 32 × 48）之外。这里按实屏 32 路收口。
 */

/* 左半屏（pixel_map x 0..63）：每模块 4 路串行数据 */
static const channel_info_t s_channel_map[TOTAL_CHANNELS] = {
    /* 模组第 1 排（显示 y 0..7） */
    {PA_IDX, LED_CH0_Pin},
    {PA_IDX, LED_CH1_Pin},
    {PA_IDX, LED_CH2_Pin},
    {PC_IDX, LED_CH3_Pin},
    /* 模组第 2 排（显示 y 8..15） */
    {PF_IDX, LED_CH12_Pin},
    {PF_IDX, LED_CH13_Pin},
    {PF_IDX, LED_CH14_Pin},
    {PC_IDX, LED_CH15_Pin},
    /* 模组第 3 排（显示 y 16..23） */
    {PD_IDX, LED_CH24_Pin},
    {PD_IDX, LED_CH25_Pin},
    {PD_IDX, LED_CH26_Pin},
    {PD_IDX, LED_CH27_Pin},
    /* 模组第 4 排（显示 y 24..31） */
    {PC_IDX, LED_CH36_Pin},
    {PG_IDX, LED_CH37_Pin},
    {PG_IDX, LED_CH38_Pin},
    {PG_IDX, LED_CH39_Pin},

    /* 右半屏（pixel_map x 64..127）：同为 4 路/模块，但引脚序在块内倒序 */
    /* 模组第 1 排（显示 y 0..7） */
    {PF_IDX, LED_CH9_Pin},
    {PF_IDX, LED_CH8_Pin},
    {PF_IDX, LED_CH7_Pin},
    {PF_IDX, LED_CH6_Pin},
    /* 模组第 2 排（显示 y 8..15） */
    {PE_IDX, LED_CH21_Pin},
    {PE_IDX, LED_CH20_Pin},
    {PE_IDX, LED_CH19_Pin},
    {PE_IDX, LED_CH18_Pin},
    /* 模组第 3 排（显示 y 16..23） */
    {PA_IDX, LED_CH33_Pin},
    {PC_IDX, LED_CH32_Pin},
    {PD_IDX, LED_CH31_Pin},
    {PD_IDX, LED_CH30_Pin},
    /* 模组第 4 排（显示 y 24..31） */
    {PD_IDX, LED_CH45_Pin},
    {PD_IDX, LED_CH44_Pin},
    {PD_IDX, LED_CH43_Pin},
    {PG_IDX, LED_CH42_Pin},
};

/* ---- P15 实例 ---- */
typedef struct {
    dev_display_t base; /**< 基类必须是首成员 */
} dev_p15_t;

[[gnu::section(".ccmram")]] static uint8_t s_pixel_map[BUFFER_SIZE];
[[gnu::section(".ccmram")]] static uint8_t s_hub75_buff[BUFFER_SIZE];

/* BSRR 预计算表：[时序步][端口]。
 *
 * **不做双缓冲**：本框架下 prepare 与 scan 由同一个 _scan_task 顺序调用
 * （dev_display.c：dirty 时先 prepare，紧接着 scan），不存在"一边填一边读"的
 * 并发；P10 驱动也据此去掉了 B 原版的双 bank。若日后 prepare 与 scan 被拆到
 * 不同上下文，必须把双缓冲加回来。
 *
 * 384 × 7 × 4B = 10752B CCMRAM。 */
[[gnu::section(".ccmram")]] static uint32_t s_hub75_io[TIMING_STEPS][USED_PORT_COUNT];

/* 静态扫描：一帧只有一行，故没有 P10 那样的 [扫行] 维。 */
static dev_p15_t s_p15 = {
    .base = {
        .ops                 = nullptr,             /* 由 dev_p15_64x8_1000001090_init 设置 */
        .module_rows         = MODULE_PIXEL_ROW,    /* 64：模组长轴（屏宽方向） */
        .module_cols         = MODULE_PIXEL_COL,    /* 8：模组短轴（屏高方向） */
        .channels_per_module = CHANNELS_PER_MODULE, /* 4 */
        .modules_per_row     = MODULES_PER_ROW,     /* 2：左列 + 右列 */
        .modules_per_col     = MODULES_PER_COL,     /* 4：排数 */
        .scan_lines          = SCAN_LINES,          /* 1：静态扫描 */
        /* 1R1G1B 全彩灯珠：8 色全支持 */
        .supported_color_mask = DEV_DISPLAY_COLOR_ALL,
        .screen_rows         = SCREEN_ROWS,      /* 128：屏宽 */
        .screen_cols         = SCREEN_COLS,      /* 32：屏高 */
        .total_channels      = TOTAL_CHANNELS,   /* 32：模组总数 × 每模组通道数 */
        .channel_pixels      = CHANNEL_PIXELS,   /* 128：每通道像素（2 行 × 64） */
        .scan_line_pixels    = SCAN_LINE_PIXELS, /* 128：静态扫描一帧就是全部像素 */
        .buffer_size         = BUFFER_SIZE,      /* 4096 */
        .pixel_map           = s_pixel_map,
        .hub75_buff          = s_hub75_buff,
        .light_level         = 7,
    },
};

/* ================================================================
 *  _convert_pixelmap: pixel_map → hub75_buff（通道优先的颜色缓冲）
 *
 *  布局：hub75_buff[ch * CHANNEL_PIXELS + 行内偏移]，
 *        行内偏移 = 通道内行（0/1）× 64 + 模组内列（0..63）。
 *
 *  这一层只表达「哪个像素归哪一路、在通道里排第几个」，**不掺任何移位/倒序**
 *  —— 位序是 BSRR 表的职责（见 _prepare_send_buffer）。
 *
 *  ⚠ `app_test_led_mapping`（直写 hub75_buff、把 dirty 压成 false）对表驱动
 *  不适用：本驱动的 prepare 每次都会用 pixel_map 重写 hub75_buff，直接写
 *  hub75_buff 既不会被 prepare 采纳、也不会重建表。灯序确认走
 *  `app_test_pixel_scan`（逐像素写 pixel_map → dirty → prepare 重建表）。
 *
 *  通道号 = 半屏 × 16 + 模组排 × 4 + 模组内行对；
 *  即一个通道覆盖显示 y 的连续两行（老固件 fixBuf 的 channel = y/2）。
 * ================================================================ */

static void _convert_pixelmap(dev_display_t *dev)
{
    for (uint16_t y = 0; y < dev->screen_cols; y++) {
        const uint8_t mod_row = (uint8_t)(y / MODULE_PIXEL_COL); /* 0..3 */
        const uint8_t row_in  = (uint8_t)(y % MODULE_PIXEL_COL); /* 模组内行 0..7 */
        const uint8_t ch_row  = (uint8_t)(row_in / ROWS_PER_CHANNEL); /* 通道 0..3 */
        const uint8_t ch_line = (uint8_t)(row_in % ROWS_PER_CHANNEL); /* 通道内行 0/1 */

        for (uint16_t x = 0; x < dev->screen_rows; x++) {
            const uint8_t half = (x < MODULE_PIXEL_ROW) ? 0U : 1U;
            const uint8_t ch   = (uint8_t)(half * CHANNELS_PER_HALF + mod_row * CHANNELS_PER_MODULE + ch_row);
            const uint8_t xi   = (uint8_t)(x - (half ? MODULE_PIXEL_ROW : 0U));

            dev->hub75_buff[(uint16_t)ch * CHANNEL_PIXELS + (uint16_t)ch_line * MODULE_PIXEL_ROW + xi]
                = dev->pixel_map[(uint32_t)y * dev->screen_rows + x];
        }
    }
}

/* ================================================================
 *  _prepare_send_buffer: hub75_buff → BSRR 整帧表（384 时序步）
 *
 *  一步一步解码出「这一步要往每路送哪一位」，再按端口攒 BSRR 字。
 *  步骤语义**逐字保留老固件 lamp_scan()**：
 *
 *    老 fixBuf 的通道 = 48 字节，字节号 i = 6g + k
 *        g = 0..7   字节列（每列 8 像素）
 *        k = 0..5   R0,R1,G0,G1,B0,B1（颜色 × 通道内行）
 *    lamp_scan 按 i = 47 → 0 发送，每个字节 MSB（bit7）先出，故：
 *        step = (47 - i) * 8 + j        j = 0..7（步内位号）
 *        被测位 = 字节的 bit(7-j)
 *
 *  位→像素的对应还要再过一道老固件 displayDataUpdata 的位序：
 *        - 行 0：字节 bit(7 - x%8)，故 bit(7-j) ↔ x%8 = j
 *        - 行 1：字节 bit(x%8)  ，故 bit(7-j) ↔ x%8 = 7-j   （行 1 反向）
 *        - 左半屏字节列倒序（缓冲组号 = 7 - x/8），右半屏正序
 *  合起来就是下面 xb / xi_left / xi_right 三行。
 * ================================================================ */

/** @brief 按**端口**分好组的通道条目（启动时建一次）
 *
 *  prepare 的内层是 384 × 32 = 12,288 次迭代。按端口分组后每个时序步每端口
 *  只写一次表，内层只剩「读一个字节 → 选一张掩码 → 或进累加器」，且整张表
 *  不需要 memset（每个端口字都由 set/rst 位拼满）。与 P10 驱动同一手法。
 */
typedef struct {
    uint32_t set;  /**< 置位字（= 引脚掩码） */
    uint32_t rst;  /**< 复位字（= 引脚掩码 << 16） */
    uint16_t off;  /**< 该通道在 hub75_buff 里的起始偏移 = ch * CHANNEL_PIXELS */
    uint8_t  half; /**< 0 = 左半屏（字节列倒序），1 = 右半屏（正序） */
    uint8_t  pad;
} p15_ch_t;

#define P15_PORT_MAX_CH (16U) /**< 单端口最多几路（本屏 PD 上 9 路，留余量） */
static p15_ch_t s_port_ch[USED_PORT_COUNT][P15_PORT_MAX_CH];
static uint8_t  s_port_cnt[USED_PORT_COUNT];

static void _build_channel_table(void)
{
    for (uint8_t p = 0; p < USED_PORT_COUNT; p++) s_port_cnt[p] = 0;

    for (uint8_t ch = 0; ch < TOTAL_CHANNELS; ch++) {
        const uint8_t p = (uint8_t)s_channel_map[ch].port_idx;
        if (p >= USED_PORT_COUNT || s_port_cnt[p] >= P15_PORT_MAX_CH) continue;

        p15_ch_t *e = &s_port_ch[p][s_port_cnt[p]++];
        e->set      = s_channel_map[ch].pin;
        e->rst      = (uint32_t)s_channel_map[ch].pin << 16;
        e->off      = (uint16_t)(ch * CHANNEL_PIXELS);
        e->half     = (ch >= CHANNELS_PER_HALF) ? 1U : 0U;
    }
}

static void _prepare_send_buffer(dev_display_t *dev)
{
    const uint8_t *buff = dev->hub75_buff;

    for (uint16_t step = 0; step < TIMING_STEPS; step++) {
        /* 老 lamp_scan 的（字节号 i, 步内位号 j） */
        const uint8_t i = (uint8_t)((BYTES_PER_CHANNEL - 1U) - step / 8U); /* 47..0 */
        const uint8_t j = (uint8_t)(step % 8U);                            /* 0..7 */

        const uint8_t grp  = (uint8_t)(i / 6U);   /* 字节列 0..7 */
        const uint8_t sub  = (uint8_t)(i % 6U);   /* 0=R0 1=R1 2=G0 3=G1 4=B0 5=B1 */
        const uint8_t row  = (uint8_t)(sub & 1U); /* 通道内行 0/1 */
        const uint8_t cbit = (uint8_t)(sub >> 1U); /* 颜色位：0=R 1=G 2=B（bit0/1/2） */

        /* 字节内像素号：行 1 的反序见上面 displayDataUpdata 的位序说明 */
        const uint8_t xb = (row == 0U) ? j : (uint8_t)(7U - j);

        const uint16_t row_off  = (uint16_t)row * MODULE_PIXEL_ROW;
        const uint16_t xi_left  = (uint16_t)(((7U - grp) * 8U) + xb); /* 左半：字节列倒序 */
        const uint16_t xi_right = (uint16_t)((grp * 8U) + xb);        /* 右半：字节列正序 */

        /* 逐端口：一个累加器攒齐该端口的全部通道，**整字写一次** */
        for (uint8_t p = 0; p < USED_PORT_COUNT; p++) {
            const p15_ch_t *c    = s_port_ch[p];
            const p15_ch_t *last = c + s_port_cnt[p];
            uint32_t        acc  = 0;

            /* 用指针走表：-Og 下不会重算下标 */
            for (; c < last; c++) {
                const uint16_t xi = c->half ? xi_right : xi_left;
                acc |= ((buff[c->off + row_off + xi] >> cbit) & 1U) ? c->set : c->rst;
            }
            s_hub75_io[step][p] = acc;
        }
    }
}

/* ---- prepare: 像素重排 + 预计算 ---- */
static void _prepare(dev_display_t *dev)
{
    _convert_pixelmap(dev);
    _prepare_send_buffer(dev);
}

/* ================================================================
 *  scan: 整帧输出 — 384 步 × 7 端口 BSRR 推送 + CLK 脉冲
 *
 *  静态扫描没有行切换：一帧 = 全部 384 位。框架每拍调用一次，OE/LAT 的
 *  原子窗口在返回后统一做（_scan_task）——移位期间锁存器保持上一帧，
 *  所以"边移边显示"不会出现残影。
 * ================================================================ */

static void _scan(dev_display_t *dev, uint8_t line)
{
    (void)dev;
    (void)line; /* 静态扫描：只有一行 */

    const uint32_t *pData = &s_hub75_io[0][0];

    for (uint16_t step = 0; step < TIMING_STEPS; step++) {
        for (uint8_t i = 0; i < USED_PORT_COUNT; i++) {
            if (*pData) pl_hub75_port_by_idx(i)->BSRR = *pData;

            pData++;
        }

        /* CLK 一个脉冲，LED 驱动芯片移位寄存器移位 */
        pl_hub75_clock_pulse();
    }
}

/* ================================================================
 *  set_row: 静态扫描没有行地址
 *
 *  A/B/C/D 恒为 0（= 8 行全部由数据线常亮驱动）。仍然显式写一遍：框架在
 *  OE 消隐的原子窗口里调用它，可以顺带清掉任何残留的地址位。若给 nullptr
 *  也能工作（pl_hub75_init 已把四根线拉低），但显式实现让"这块屏没有行
 *  地址"这件事在虚表里可见。
 * ================================================================ */

static void _set_row(uint8_t row)
{
    (void)row;

    HUB75_A = 0;
    HUB75_B = 0;
    HUB75_C = 0;
    HUB75_D = 0;
}

/* ---- ops 虚表 ---- */
static const dev_display_ops_t s_p15_ops = {
    .prepare = _prepare,
    .scan    = _scan,
    .set_row = _set_row,
};

/**
 * @brief P15 64×8 模组初始化：展开通道表、绑定 ops、注册为活动显示实例
 *
 * 由 hw_dev_initcall 在 RTOS 启动前调用。同一块板上**只能编译一个**显示驱动：
 * 每个驱动都会 dev_display_register()，后注册者覆盖前者。
 */
void dev_p15_64x8_1000001090_init(void)
{
    _build_channel_table(); /* 通道 → 端口/掩码 展开一次，prepare 内层不再查表 */
    s_p15.base.ops = &s_p15_ops;
    dev_display_register(&s_p15.base);
}
hw_dev_initcall(dev_p15_64x8_1000001090_init);

/* ================================================================
 *  移植说明（排障时先读这一节）
 *
 *  1. 为什么时序 1:1 照抄老固件
 *     老工程 P15_flip_BarBoard_BB 的 lamp_scan() 虽然结构粗糙（20 路/半屏、
 *     越界读 fixBuf、通道号与 6 字节组对不上），但它是这块屏**唯一上过机**
 *     的字节序/位序依据：字节号 47→0、每字节 MSB 先出、行 1 反向、左半屏
 *     字节列倒序。没有硬件资料能推翻它之前，重写只换结构不换时序。
 *
 *  2. 40 路 → 32 路
 *     控制卡按 5 个 12 线块布线（每块两个 HUB75 接口 × 6 条颜色线），一块
 *     模组只用 4 条串行线。P15 屏 4 排 = 32 路；第 5 块对应不存在的第 5 排：
 *     老代码左半屏那 4 路读到右半屏的数据，右半屏那 4 路读到 fixBuf 之外
 *     （1536B 之后）。本驱动不再扫。
 *
 *  3. 上机验证顺序（host 测试覆盖不到真实时序）
 *     a. 全绿/全红/全蓝：确认三色通道与 OE 极性（app_test 的逐色老化）。
 *     b. app_test_pixel_scan：逐像素走一遍，确认 32 路都通、无错位。
 *     c. 四角点亮：确认半屏/模组排的拼接方向（左半屏字节列倒序最容易在这里
 *        暴露 —— 症状是左半屏左右镜像）。
 *     d. 若 c 出现"左半屏镜像"，先把 _prepare_send_buffer 里的 xi_left 改成
 *        grp * 8 + xb（与右半同向）复测；若出现"行 1 与行 0 交错"，检查 xb
 *        的 row 分支（行 1 反序）。
 *     e. 若怀疑某一路接线与表不符（单点/整条不亮、串位），按 s_channel_map
 *        逐条对调复测 —— 这是唯一需要改的硬件事实表。
 *
 *  4. 帧率
 *     框架每拍调一次 scan（本板 TIM3 = 200Hz），静态屏不需要刷新，但整帧
 *     重发让移位寄存器被周期性重写，抗干扰/自愈 —— 一帧 384 步的 CPU 占用
 *     约 1%（-Og，168MHz）。如日后要省，可在 _scan 里按"表未变则跳过"裁剪，
 *     但必须保留每拍一次的 OE/LAT 窗口。
 * ================================================================ */
