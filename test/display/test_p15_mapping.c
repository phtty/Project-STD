/**
 * @file    test_p15_mapping.c
 * @brief   P15 64×8 静态扫描：像素 → BSRR 位流，必须逐位等于老 lamp_scan
 *
 * **为什么需要这个测试**：老工程 `P15_flip_BarBoard_BB/USER/TIMER/timer.c` 的
 * `lamp_scan()` 是这块屏**唯一上过机**的时序依据（字节 47→0、每字节 MSB 先出、
 * 行 1 反序、左半屏字节列倒序），但它同时有"20 路/半屏、越界读"的毛病，只能
 * 对照着重写。重写之后，"到底哪一位对应哪个像素"没有任何运行期自检会报错 ——
 * 错了的唯一表现是屏上镜像/串位，得装箱上电才看得见。
 *
 * 所以这里把**老实现当参照模型**（oracle）：
 *   1) 按老 `displayDataUpdata()` 的规则（含它的 LED_Color 枚举顺序、行 0/行 1
 *      的两种位序、左右半屏的字节列方向）把 pixel_map 组回老 fixBuf（32×48 字节）；
 *   2) 按老 `lamp_scan()` 的发送顺序（字节 47→0、每个字节 bit7 先出）展开成
 *      32 通道 × 384 步的位流。
 *
 * 被测侧跑的是**生产驱动本体**（TU-include，不改写、不替换）：从它预计算的
 * BSRR 表里，**按老 lamp_scan 的引脚顺序**逐位反解，与参照位流逐位比对。
 *
 * 两条路径刻意异写：参照侧显式建字节缓冲再按发送序取位；被测侧是"解码时序步 →
 * 取像素颜色位"。引脚表是硬件事实，两边**各持一份**（参照侧故意不读被测的
 * `s_channel_map`）—— 这样驱动里引脚写错、通道顺序写反都会被抓到。
 *
 * 覆盖：
 *   · 几何/虚表/初始化注册；
 *   · 32 路引脚逐一对照（含"同一 (端口,位) 不得被两路共用"）；
 *   · 全黑 / 全白 / 三原色：位流 + 点亮的位数（与位置无关的不变量）；
 *   · 四角/边界单点：一个像素只能点亮 1 位（白 3 位）—— 老代码的
 *     "同一引脚写两次 / 越界读到邻区"都会让这个数不对；
 *   · 8 帧确定性随机图：逐位对照；
 *   · `scan` 一帧 384 个 CLK、`set_row` 把 A~D 拉低。
 *
 * 本套件在 3833024 与 5006048 两种构建下**都会跑**：包含路径固定换成
 * `test/stubs/host_p15/` 的替身头，与 `BOARD` 宏无关（被测的是 5006048 的驱动源码）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 真 dev_display.h（带 ops 虚表）。本套件的包含路径**不用** test/stubs 里的
   精简影子 —— 见 Makefile 的 TEST_P15MAP_CFLAGS。 */
#include "dev_display.h"

/* ================================================================
 *  host 夹具：假 GPIO / 控制线影子 / 时钟计数
 * ================================================================ */

GPIO_TypeDef g_p15_host_gpio[7];
uint8_t      g_p15_host_bit[7][16];
uint16_t     g_p15_host_clk_pulses;

GPIO_TypeDef *pl_hub75_port_by_idx(uint8_t idx)
{
    return (idx < 7U) ? &g_p15_host_gpio[idx] : NULL;
}

void pl_hub75_init(void)
{
}

/* 驱动初始化会调它；本套件只关心"注册的是谁" */
static dev_display_t *s_registered;

void dev_display_register(dev_display_t *dev)
{
    s_registered = dev;
}

/* ================================================================
 *  生产驱动本体（TU-include：BSRR 表、通道表、prepare/scan 都是 static）
 * ================================================================ */

#include "../../boards/5006048/Device/Src/dev_p15_64x8_1000001090.c"

/* ================================================================
 *  参照模型（oracle）—— 老工程的 displayDataUpdata + lamp_scan
 * ================================================================ */

#define LEGACY_CH_BYTES 48U /**< 老 fixBuf 每个通道的字节数 */
#define LEGACY_STEPS    384U /**< 每通道发送位数 = 48 × 8 */

/** @brief 参照引脚表：老 lamp_scan 的通道 → (端口, 位)
 *
 *  老代码每半屏写 20 路（5 个 12 线块），P15 屏只有 4 排 → 取前 4 块共 32 路。
 *  顺序 = 缓冲通道序：
 *    左半 0..15：CH0..3 / CH12..15 / CH24..27 / CH36..39
 *    右半 0..15：CH9..6 / CH21..18 / CH33..30 / CH45..42（块内倒序）
 *
 *  **故意不引用被测的 s_channel_map**：那样驱动写错引脚就测不出来了。 */
typedef struct {
    uint8_t  port; /**< 0..6 = PA..PG（与 pl_hub75_port_by_idx 同序） */
    uint16_t pin;  /**< 引脚掩码 */
} oracle_pin_t;

enum {
    ORACLE_PA = 0,
    ORACLE_PB,
    ORACLE_PC,
    ORACLE_PD,
    ORACLE_PE,
    ORACLE_PF,
    ORACLE_PG,
};

static const oracle_pin_t s_oracle_pins[32] = {
    /* 左半屏：块内偏移 0..3（正序） */
    {ORACLE_PA, LED_CH0_Pin},  {ORACLE_PA, LED_CH1_Pin},  {ORACLE_PA, LED_CH2_Pin},
    {ORACLE_PC, LED_CH3_Pin},  {ORACLE_PF, LED_CH12_Pin}, {ORACLE_PF, LED_CH13_Pin},
    {ORACLE_PF, LED_CH14_Pin}, {ORACLE_PC, LED_CH15_Pin}, {ORACLE_PD, LED_CH24_Pin},
    {ORACLE_PD, LED_CH25_Pin}, {ORACLE_PD, LED_CH26_Pin}, {ORACLE_PD, LED_CH27_Pin},
    {ORACLE_PC, LED_CH36_Pin}, {ORACLE_PG, LED_CH37_Pin}, {ORACLE_PG, LED_CH38_Pin},
    {ORACLE_PG, LED_CH39_Pin},
    /* 右半屏：块内偏移 6..9（倒序） */
    {ORACLE_PF, LED_CH9_Pin},  {ORACLE_PF, LED_CH8_Pin},  {ORACLE_PF, LED_CH7_Pin},
    {ORACLE_PF, LED_CH6_Pin},  {ORACLE_PE, LED_CH21_Pin}, {ORACLE_PE, LED_CH20_Pin},
    {ORACLE_PE, LED_CH19_Pin}, {ORACLE_PE, LED_CH18_Pin}, {ORACLE_PA, LED_CH33_Pin},
    {ORACLE_PC, LED_CH32_Pin}, {ORACLE_PD, LED_CH31_Pin}, {ORACLE_PD, LED_CH30_Pin},
    {ORACLE_PD, LED_CH45_Pin}, {ORACLE_PD, LED_CH44_Pin}, {ORACLE_PD, LED_CH43_Pin},
    {ORACLE_PG, LED_CH42_Pin},
};

/** @brief 新颜色编码（bit0=R / bit1=G / bit2=B）→ 老工程的 LED_Color 枚举值
 *
 *  **两套枚举顺序不同**：老工程是 black=0, green=1, red=2, yellow=3, white=4,
 *  blue=5, purple=6, cyan=7。参照侧必须走这道翻译，否则红绿会整屏互换 ——
 *  这正是"照抄老代码"最容易带进来的坑。 */
static uint8_t _legacy_color_code(uint8_t rgb)
{
    static const uint8_t map[8] = {
        0, /* 0 全灭 → black */
        2, /* 1 R    → red */
        1, /* 2 G    → green */
        3, /* 3 R+G  → yellow */
        5, /* 4 B    → blue */
        6, /* 5 R+B  → purple */
        7, /* 6 G+B  → cyan */
        4, /* 7 全亮 → white */
    };
    return map[rgb & 7U];
}

/** @brief 参照侧：按老 displayDataUpdata 的规则把 pixel_map 组回 fixBuf */
static void _legacy_fixbuf(const uint8_t *map, uint8_t fix[32][LEGACY_CH_BYTES])
{
    memset(fix, 0, 32U * LEGACY_CH_BYTES);

    for (uint16_t y = 0; y < 32U; y++) {
        for (uint16_t x = 0; x < 128U; x++) {
            const uint8_t rgb  = (uint8_t)(map[(uint32_t)y * 128U + x] & 7U);
            const uint8_t half = (x >= 64U) ? 1U : 0U;
            const uint8_t ch   = (uint8_t)(half * 16U + y / 2U);
            /* 老代码：左半屏组号 = 7 - x/8，右半屏 = (x-64)/8 */
            const uint8_t g    = half ? (uint8_t)((x - 64U) / 8U) : (uint8_t)(7U - x / 8U);
            const uint8_t row  = (uint8_t)(y & 1U);
            /* 老代码的位序：行 0 是 0x80 >> (x%8)，行 1 是 1 << (x%8) */
            const uint8_t mask = row ? (uint8_t)(1U << (x & 7U)) : (uint8_t)(0x80U >> (x & 7U));

            /* 老 switch：枚举值 → R/G/B 三位 */
            uint8_t bits = 0U;
            switch (_legacy_color_code(rgb)) {
                case 2: bits = 1U; break; /* red */
                case 1: bits = 2U; break; /* green */
                case 5: bits = 4U; break; /* blue */
                case 3: bits = 3U; break; /* yellow */
                case 6: bits = 5U; break; /* purple */
                case 7: bits = 6U; break; /* cyan */
                case 4: bits = 7U; break; /* white */
                default: bits = 0U; break; /* black */
            }

            for (uint8_t c = 0; c < 3U; c++) {
                if ((bits >> c) & 1U)
                    fix[ch][6U * g + 2U * c + row] |= mask;
            }
        }
    }
}

/** @brief 参照位流：老 lamp_scan 第 step 位（字节 47→0、bit7→bit0） */
static uint8_t _legacy_bit(const uint8_t fix[32][LEGACY_CH_BYTES], uint8_t ch, uint16_t step)
{
    const uint8_t i = (uint8_t)((LEGACY_CH_BYTES - 1U) - step / 8U);
    const uint8_t j = (uint8_t)(step % 8U);
    return (uint8_t)(((fix[ch][i] << j) & 0x80U) ? 1U : 0U);
}

/* ================================================================
 *  对拍
 * ================================================================ */

/** @brief 一帧的对拍结果 */
typedef struct {
    unsigned mismatches; /**< 位值不一致的次数 */
    unsigned undriven;   /**< 该位既没置位也没复位的次数（表没写全） */
    unsigned ones;       /**< 实际输出为 1 的位数 */
} frame_result_t;

/** @brief 从被测算出的 BSRR 表里按引脚反解一位
 *  @retval 0 复位 / 1 置位 / -1 该引脚在这一步既没置位也没复位
 */
static int _table_level(uint16_t step, uint8_t port, uint16_t pin)
{
    const uint32_t w   = s_hub75_io[step][port];
    const bool     set = (w & (uint32_t)pin) != 0U;
    const bool     rst = (w & ((uint32_t)pin << 16)) != 0U;

    if (set == rst) return -1; /* 都没写，或同时置位+复位 —— 都是表坏了 */
    return set ? 1 : 0;
}

/** @brief 跑一帧对拍：参照位流 vs 生产驱动的 BSRR 表
 *  @param map    128×32 的颜色（新编码）
 *  @param report true = 打印前几条不一致（排障用）
 *  @return 统计结果
 */
static frame_result_t _compare_frame(const uint8_t *map, bool report)
{
    static uint8_t fix[32][LEGACY_CH_BYTES];
    frame_result_t r = {0, 0, 0};

    _legacy_fixbuf(map, fix);

    memcpy(s_p15_dev.base.pixel_map, map, 128U * 32U);
    _p15_prepare(&s_p15_dev.base); /* 生产驱动本体 */

    for (uint8_t ch = 0; ch < 32U; ch++) {
        const uint8_t  port = s_oracle_pins[ch].port;
        const uint16_t pin  = s_oracle_pins[ch].pin;

        for (uint16_t step = 0; step < LEGACY_STEPS; step++) {
            const int got = _table_level(step, port, pin);

            if (got < 0) {
                r.undriven++;
                if (report && r.undriven <= 5U)
                    printf("      通道 %u 第 %u 步：引脚没被驱动\n", (unsigned)ch,
                           (unsigned)step);
                continue;
            }

            const uint8_t want = _legacy_bit(fix, ch, step);

            r.ones += (unsigned)got;
            if ((uint8_t)got != want) {
                r.mismatches++;
                if (report && r.mismatches <= 5U)
                    printf("      通道 %u 第 %u 步：驱动 %d，参照 %u\n", (unsigned)ch,
                           (unsigned)step, got, (unsigned)want);
            }
        }
    }
    return r;
}

/** @brief 一帧里按颜色应点亮的位数（与位置无关的不变量） */
static unsigned _expected_ones(const uint8_t *map)
{
    unsigned n = 0;
    for (uint16_t i = 0; i < 128U * 32U; i++) {
        const uint8_t v = (uint8_t)(map[i] & 7U);
        n += (unsigned)(v & 1U) + (unsigned)((v >> 1) & 1U) + (unsigned)((v >> 2) & 1U);
    }
    return n;
}

/** @brief 把一帧的每个像素都设成同一颜色后对拍 */
static frame_result_t _compare_fill(uint8_t rgb, bool report)
{
    static uint8_t map[128U * 32U];
    memset(map, rgb, sizeof map);
    return _compare_frame(map, report);
}

/* ================================================================
 *  断言（与 test_crc.c 同一变体 B：两句式断言 + TEST_BEGIN；
 *  宏定义里的参数名刻意不在注释中出现，避免 lint 的 R8 把注释当成
 *  "被注掉的断言调用"）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 初始化注册、几何、虚表；**必须是第一个用例**（后面依赖通道表已建好） */
static void case_init_and_geometry(void)
{
    TEST_BEGIN("初始化注册 / 几何 / 虚表");

    dev_p15_64x8_1000001090_init();

    const dev_display_t *d = &s_p15_dev.base;

    CHECK_MSG(s_registered == &s_p15_dev.base, "初始化没有把本实例注册为活动显示");
    CHECK_MSG(d->ops != NULL && d->ops->prepare != NULL && d->ops->scan != NULL &&
                  d->ops->set_row != NULL,
              "ops 虚表不完整");
    CHECK_MSG(d->screen_rows == 128U && d->screen_cols == 32U, "几何应为 128×32，得到 %u×%u",
              (unsigned)d->screen_rows, (unsigned)d->screen_cols);
    CHECK_MSG(d->total_channels == 32U, "通道数应为 32，得到 %u", (unsigned)d->total_channels);
    CHECK_MSG(d->scan_lines == 1U, "静态扫描 scan_lines 应为 1，得到 %u", (unsigned)d->scan_lines);
    CHECK_MSG((uint32_t)d->channel_pixels * d->total_channels == d->buffer_size,
              "每通道像素 %u × 通道 %u 应等于 buffer %u", (unsigned)d->channel_pixels,
              (unsigned)d->total_channels, (unsigned)d->buffer_size);
}

/** @brief 32 路引脚逐一对照老 lamp_scan，且同一 (端口,位) 不得被两路共用 */
static void case_channel_pins(void)
{
    TEST_BEGIN("通道 → 引脚：与老 lamp_scan 逐条一致且互不重复");

    uint8_t seen[7][16] = {{0}};
    unsigned dup = 0;

    for (uint8_t ch = 0; ch < 32U; ch++) {
        const uint8_t  p   = (uint8_t)s_channel_map[ch].port_idx;
        const uint16_t pin = s_channel_map[ch].pin;

        CHECK_MSG(p == s_oracle_pins[ch].port && pin == s_oracle_pins[ch].pin,
                  "通道 %u：驱动 %u/%04X，参照 %u/%04X", (unsigned)ch, (unsigned)p,
                  (unsigned)pin, (unsigned)s_oracle_pins[ch].port,
                  (unsigned)s_oracle_pins[ch].pin);

        /* 位掩码必须只有一位，且不能与前面某路共用 */
        if (pin != 0U && (pin & (uint16_t)(pin - 1U)) == 0U && p < 7U) {
            uint8_t bit = 0;
            while (((pin >> bit) & 1U) == 0U && bit < 16U) bit++;
            if (bit < 16U && seen[p][bit]) dup++;
            if (bit < 16U) seen[p][bit] = 1;
        }
    }

    CHECK_MSG(dup == 0U, "有 %u 路共用同一 (端口,位) —— BSRR 表将无法按引脚反解", dup);
}

/** @brief 全黑：一位都不该亮，且每路每步都必须被显式驱动 */
static void case_all_black(void)
{
    TEST_BEGIN("全黑：0 位点亮");

    const frame_result_t r = _compare_fill(0, true);

    CHECK_MSG(r.mismatches == 0U && r.undriven == 0U, "位流不一致 %u 处、未驱动 %u 处",
              r.mismatches, r.undriven);
    CHECK_MSG(r.ones == 0U, "全黑应 0 位点亮，实际 %u", r.ones);
}

/** @brief 全白：每路 384 位全亮，共 12288 位 */
static void case_all_white(void)
{
    TEST_BEGIN("全白：32 × 384 = 12288 位全亮");

    const frame_result_t r = _compare_fill(7, true);

    CHECK_MSG(r.mismatches == 0U && r.undriven == 0U, "位流不一致 %u 处、未驱动 %u 处",
              r.mismatches, r.undriven);
    CHECK_MSG(r.ones == 32U * 384U, "全白应 12288 位点亮，实际 %u", r.ones);
}

/** @brief 三原色：各应点亮 4096 位，且位流与参照一致（抓红绿枚举互换） */
static void case_primary_colors(void)
{
    TEST_BEGIN("三原色：红/绿/蓝各 4096 位");

    const frame_result_t red   = _compare_fill(1, true);
    const frame_result_t green = _compare_fill(2, true);
    const frame_result_t blue  = _compare_fill(4, true);

    CHECK_MSG(red.mismatches == 0U && green.mismatches == 0U && blue.mismatches == 0U,
              "位流不一致：红 %u、绿 %u、蓝 %u", red.mismatches, green.mismatches,
              blue.mismatches);
    CHECK_MSG(red.ones == 128U * 32U && green.ones == 128U * 32U && blue.ones == 128U * 32U,
              "点亮位数：红 %u、绿 %u、蓝 %u（各应为 4096）", red.ones, green.ones, blue.ones);
}

/** @brief 单像素：一个红点只亮 1 位、一个白点只亮 3 位（四角与左右半屏边界） */
static void case_corners(void)
{
    TEST_BEGIN("四角/边界单点：红 1 位、白 3 位");

    static const uint16_t xs[] = {0, 63, 64, 127, 0, 64};
    static const uint16_t ys[] = {0, 0, 0, 31, 31, 31};
    static uint8_t        map[128U * 32U];

    unsigned bad = 0;
    uint16_t bad_x = 0;
    uint16_t bad_y = 0;

    for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) {
        memset(map, 0, sizeof map);
        map[(uint32_t)ys[k] * 128U + xs[k]] = 1U; /* 红 */
        const frame_result_t red = _compare_frame(map, true);
        if (red.mismatches != 0U || red.undriven != 0U || red.ones != 1U) {
            if (bad == 0U) {
                bad_x = xs[k];
                bad_y = ys[k];
            }
            bad++;
        }

        map[(uint32_t)ys[k] * 128U + xs[k]] = 7U; /* 白 */
        const frame_result_t white = _compare_frame(map, true);
        if (white.mismatches != 0U || white.undriven != 0U || white.ones != 3U) {
            if (bad == 0U) {
                bad_x = xs[k];
                bad_y = ys[k];
            }
            bad++;
        }
    }

    CHECK_MSG(bad == 0U, "%u 个单点用例不对（首个 x=%u y=%u；红应 1 位、白应 3 位）", bad,
              (unsigned)bad_x, (unsigned)bad_y);
}

/** @brief 8 帧确定性随机图：逐位对照 + 点亮位数不变式 */
static void case_random_frames(void)
{
    TEST_BEGIN("8 帧随机图：逐位对照 + 点亮位数");

    static uint8_t map[128U * 32U];
    uint32_t       rng = 0x12345678U;
    unsigned       bad = 0;
    unsigned       bad_frame = 0;

    for (unsigned frame = 0; frame < 8U; frame++) {
        for (uint16_t i = 0; i < 128U * 32U; i++) {
            rng        = rng * 1664525U + 1013904223U;
            map[i]     = (uint8_t)(rng >> 29); /* 0..7 */
        }

        const frame_result_t r = _compare_frame(map, true);
        if (r.mismatches != 0U || r.undriven != 0U || r.ones != _expected_ones(map)) {
            if (bad == 0U) bad_frame = frame;
            bad++;
        }
    }

    CHECK_MSG(bad == 0U, "%u/8 帧不一致（首个失败帧 %u，位流或点亮位数）", bad, bad_frame);
}

/** @brief scan 一帧发 384 个 CLK；set_row 把 A~D 拉低 */
static void case_scan_and_set_row(void)
{
    TEST_BEGIN("scan/set_row：384 个 CLK、行地址恒低");

    /* 先把 A~D 全置 1，确认 set_row 真的把它们写回 0（而不是"本来就没动过"） */
    g_p15_host_bit[5][15] = 1; /* A */
    g_p15_host_bit[5][14] = 1; /* B */
    g_p15_host_bit[6][1]  = 1; /* C */
    g_p15_host_bit[6][0]  = 1; /* D */
    _p15_set_row(0);

    CHECK_MSG(g_p15_host_bit[5][15] == 0 && g_p15_host_bit[5][14] == 0 &&
                  g_p15_host_bit[6][1] == 0 && g_p15_host_bit[6][0] == 0,
              "set_row 之后 A~D 应全为 0");

    g_p15_host_clk_pulses = 0;
    _p15_scan(&s_p15_dev.base, 0);

    CHECK_MSG(g_p15_host_clk_pulses == 384U, "一帧应发 384 个 CLK，实际 %u",
              (unsigned)g_p15_host_clk_pulses);
}

/* ================================================================ */

int main(void)
{
    case_init_and_geometry();
    case_channel_pins();
    case_all_black();
    case_all_white();
    case_primary_colors();
    case_corners();
    case_random_frames();
    case_scan_and_set_row();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
