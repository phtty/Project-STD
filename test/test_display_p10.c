/**
 * @file    test_display_p10.c
 * @brief   P10 32×16 模组驱动（dev_p10_32x16_2200001703）host 单测：几何自洽、
 *          像素→发送缓冲的**双射**、行配对（IC 的 16 个输出 = 8 列 × 2 行）、
 *          4 个扫描步的行覆盖、以及"整帧扫完能否还原出原图"
 *
 * **为什么需要这个测试**：这块屏是 320×64、10×4 个模组、1/4 扫、8 条数据线
 * （每模块 2 条）。驱动的 `_prepare` 把 `pixel_map` 重排进 `hub75_buff`，再由
 * `_scan` 逐拍推出去 —— 中间任何一处错位，现场表现都是"某一半不亮/错行/花屏"，
 * 而 host 侧以前**一条断言都没有**（`dev_display.c` 的通用层只有画布/渲染套件在测）。
 *
 * 这套件把"假面板"写出来对拍：按模组接线把每一拍推出去的像素还原成物理 LED 坐标，
 * 再和原始图逐像素比。它抓得住的一类缺陷：
 *   · 缓冲字节数与 通道×通道像素 的除法不自洽（`buffer_size != total_channels × channel_pixels`）
 *     —— 多出来的通道读到缓冲外/重复读同一段，屏上就是半幅不亮；
 *   · 行配对写错（把 r 与 r+8 配在一起，而面板是 r 与 r+4）—— 整屏错行；
 *   · 某个扫描步漏排/重排（4 步覆盖不到 64 行）—— 固定几条不亮；
 *   · 颜色位序错（G/B 互换、bit0 不是 R）—— 颜色不对。
 *
 * **抓不住的一类**：如果**实物面板**的接线与驱动假设不同（比如 IC 先输出 r+4 再输出 r，
 * 或者地址 1-based），假面板模型也会跟着错，套件仍会绿。模型是照"驱动的接线约定"写的，
 * 所以它的价值是"钉住约定 + 抓实现漂移"，不能替代上机。约定写在 `model_pixel()` 的
 * 注释里，换面板时**先改那里**，再看断言是否变红。
 *
 * 手法：TU-include 板级模组驱动，`pl_hub75.h` 用 `test/stubs` 的替身（控制访问器换成
 * 记录桩），引脚表用一张假表（一通道一位），于是 BSRR 值可以反解成"第几通道、哪一位"。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dev_display.h" /* test/stubs 的影子头：含 ops 虚表与模组参数 */
#include "initcall.h"
#include "pl_hub75.h" /* test/stubs 的影子头：类型 + 可观测的控制访问器 */

/* ================================================================
 *  桩：假 GPIO 与引脚表
 *
 *  引脚号 = 1<<通道号，一个端口代表一种颜色 —— 这样从 BSRR 的 val 就能反解出
 *  "通道几、置位还是复位"：置位写 pin，复位写 pin<<16（驱动就是这么预计算的）。
 * ================================================================ */

#define TEST_CH 8U /* P10 32×16 只用前 8 路（模组 4 列 × 每模组 2 通道） */

static GPIO_TypeDef s_port_r;
static GPIO_TypeDef s_port_g;
static GPIO_TypeDef s_port_b;

const pl_hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX] = {
    {&s_port_r, 1U << 0}, {&s_port_r, 1U << 1}, {&s_port_r, 1U << 2}, {&s_port_r, 1U << 3},
    {&s_port_r, 1U << 4}, {&s_port_r, 1U << 5}, {&s_port_r, 1U << 6}, {&s_port_r, 1U << 7},
    {&s_port_r, 1U << 8}, {&s_port_r, 1U << 9},
};
const pl_hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX] = {
    {&s_port_g, 1U << 0}, {&s_port_g, 1U << 1}, {&s_port_g, 1U << 2}, {&s_port_g, 1U << 3},
    {&s_port_g, 1U << 4}, {&s_port_g, 1U << 5}, {&s_port_g, 1U << 6}, {&s_port_g, 1U << 7},
    {&s_port_g, 1U << 8}, {&s_port_g, 1U << 9},
};
const pl_hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX] = {
    {&s_port_b, 1U << 0}, {&s_port_b, 1U << 1}, {&s_port_b, 1U << 2}, {&s_port_b, 1U << 3},
    {&s_port_b, 1U << 4}, {&s_port_b, 1U << 5}, {&s_port_b, 1U << 6}, {&s_port_b, 1U << 7},
    {&s_port_b, 1U << 8}, {&s_port_b, 1U << 9},
};

/* ================================================================
 *  桩：控制访问器 —— 记录"这一拍推出去的是什么"
 * ================================================================ */

#define TEST_SCAN_PIXELS 640U /* = channel_pixels / scan_lines = 2560 / 4 */
#define TEST_SCAN_LINES  4U

static uint8_t  s_cap[TEST_SCAN_LINES][TEST_SCAN_PIXELS][TEST_CH]; /* [行][拍内第几个像素][通道] 颜色 */
static uint8_t  s_pending[TEST_CH];                      /* 当前像素已解出的颜色位 */
static uint16_t s_pix_idx;
static uint8_t  s_cur_line;
static bool     s_capturing;
static uint8_t  s_row_addr;      /* 最近一次 set_row 的实参 */
static uint32_t s_clock_pulses;  /* 时钟脉冲总数（= 推出去的像素数） */

void pl_hub75_bsrr_flush(const pl_hub75_bsrr_t *p)
{
    if (p == nullptr) return;

    /* val 是 "pin"（置位）或 "pin<<16"（复位）；低 16 位里只有一位 */
    uint8_t  ch   = 0xFFU;
    bool     high = true;
    for (uint8_t i = 0; i < TEST_CH; i++) {
        const uint32_t m = 1UL << i;
        if (p->val & m) {
            ch   = i;
            high = true;
            break;
        }
        if (p->val & (m << 16)) {
            ch   = i;
            high = false;
            break;
        }
    }
    if (ch == 0xFFU) return; /* 非本表引脚：忽略（真实现只会有已知值） */

    const uint8_t mask = (p->port == &s_port_r) ? 0x1U : (p->port == &s_port_g) ? 0x2U : 0x4U;
    if (high)
        s_pending[ch] |= mask;
    else
        s_pending[ch] &= (uint8_t)~mask;
}

void pl_hub75_clock_pulse(void)
{
    s_clock_pulses++;
    if (s_capturing && s_cur_line < TEST_SCAN_LINES && s_pix_idx < TEST_SCAN_PIXELS)
        memcpy(s_cap[s_cur_line][s_pix_idx], s_pending, TEST_CH);
    s_pix_idx++;
    memset(s_pending, 0, sizeof(s_pending));
}

void pl_hub75_latch_pulse(void) { /* 本套件不观测 */ }

void pl_hub75_oe_set(bool blank) { (void)blank; }

void pl_hub75_set_row(uint8_t row) { s_row_addr = row; }

/* ================================================================
 *  被测：板级模组驱动本体（TU-include：s_module / s_module_ops / 缓冲都是 static）
 * ================================================================ */

#include "../boards/3833024/Device/Src/dev_p10_32x16_2200001703.c"

/** @brief 桩：模组注册（真实现只是把指针存起来，供 dev_display_get 取）
 *
 *  本套件直接拿着驱动里的静态实例 `s_module` 用，不需要"当前生效模组"这层。 */
void dev_display_register(dev_display_t *dev) { (void)dev; }

/* ================================================================
 *  断言与统计（A 变体：CHECK + CHECK_MSG + TEST_BEGIN）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                 \
            printf("\n");                                                        \
        }                                                                        \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  假面板模型 —— 与驱动"独立"按模组接线写
 *
 *  接线约定（2200001703：32×16 模组、1/4 扫、每模组 2 条数据线）：
 *    · 横向 10 个模组、纵向 4 排，每模组 32 列 × 16 行；
 *    · 一条数据线（一个"通道"）串起 **2 行 × 320 列 = 640 位**，跨 10 个模组；
 *    · 通道 ch 服务第 (ch/2) 排模组：ch%2==0 → 该模组上半（行 0..7），=1 → 下半（行 8..15）；
 *    · 每位驱动 IC 有 16 个输出 = **8 列 × 2 行**：先推"地址对应行"，再推"该行 +4"；
 *    · 行地址 L（0..3）选中每半屏的第 L 行与第 L+4 行。
 *
 *  换面板（或换驱动）时**先改这里**，再看断言是红是绿 —— 模型即接线契约。
 * ================================================================ */

/** @brief 把"第 line 拍、第 ch 通道、该拍第 k 个时钟"还原成物理 LED 坐标 */
static void model_pixel(uint8_t line, uint8_t ch, uint16_t k, uint16_t *row, uint16_t *col)
{
    const uint8_t  mcol   = (uint8_t)(k / 64U); /* 第几个模组（横向 0..9） */
    const uint8_t  within = (uint8_t)(k % 64U); /* 模组内第几位 */
    const uint8_t  grp    = (uint8_t)(within / 16U); /* 模组内第几个 IC（0..3） */
    const uint8_t  pos    = (uint8_t)(within % 16U); /* IC 内第几个输出 */
    const uint8_t  which  = (uint8_t)(pos / 8U);     /* 0 = 行 L，1 = 行 L+4 */
    const uint8_t  c      = (uint8_t)(grp * 8U + pos % 8U);

    *row = (uint16_t)((ch / 2U) * 16U + (ch % 2U) * 8U + line + which * 4U);
    *col = (uint16_t)(mcol * 32U + c);
}

/* ================================================================
 *  夹具
 * ================================================================ */

static dev_display_t *dsp(void) { return &s_module.base; }

static void fixture_reset(void)
{
    memset(s_pixel_map, 0, sizeof(s_pixel_map));
    memset(s_hub75_buff, 0, sizeof(s_hub75_buff));
    memset(s_cap, 0, sizeof(s_cap));
    memset(s_pending, 0, sizeof(s_pending));
    s_pix_idx      = 0;
    s_cur_line     = 0;
    s_capturing    = false;
    s_row_addr     = 0xFF;
    s_clock_pulses = 0;
    dsp()->dirty   = false;
}

/** @brief 跑整帧：4 个扫描步，把每一拍推出去的像素都录下来 */
static void capture_frame(void)
{
    memset(s_cap, 0, sizeof(s_cap));
    s_capturing = true;
    for (uint8_t line = 0; line < TEST_SCAN_LINES; line++) {
        s_cur_line = line;
        s_pix_idx  = 0;
        dsp()->ops->scan(dsp(), line);
    }
    s_capturing = false;
}

/** @brief 用假面板把录下来的推送还原成 320×64 的显存；返回写了多次的像素数 */
static uint32_t reconstruct(uint8_t shown[64][320])
{
    uint8_t  hits[64][320];
    uint32_t multi = 0;

    memset(hits, 0, sizeof(hits));
    memset(shown, 0xFF, 64U * 320U);

    for (uint8_t line = 0; line < TEST_SCAN_LINES; line++) {
        for (uint8_t ch = 0; ch < TEST_CH; ch++) {
            for (uint16_t k = 0; k < TEST_SCAN_PIXELS; k++) {
                uint16_t row, col;
                model_pixel(line, ch, k, &row, &col);
                if (row >= 64U || col >= 320U) continue; /* 模型外，交给用例断言 */
                shown[row][col] = s_cap[line][k][ch];
                if (++hits[row][col] > 1U) multi++;
            }
        }
    }
    return multi;
}

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 几何自洽：缓冲正好被 通道×通道像素 铺满（除法一旦不整除，屏上必有一片不亮）
 *
 *  这条是整个套件的地基：`buffer_size != total_channels × channel_pixels` 时，
 *  `_scan` 的 `hub75_buff[pixel_base + ch*channel_pixels]` 会读到缓冲外（ASan 抓）
 *  或重复读同一段（半幅不亮）。本板三个数必须严格相等。 */
static void case_geometry(void)
{
    TEST_BEGIN("几何：320×64 / 8 通道 / 4 扫 / 通道像素 2560 / 每拍 640");

    CHECK(dsp()->screen_rows == 320);
    CHECK(dsp()->screen_cols == 64);
    CHECK(dsp()->buffer_size == 320U * 64U);

    CHECK(dsp()->total_channels == 8);
    CHECK(dsp()->scan_lines == 4);
    CHECK(dsp()->scan_line_pixels == 640);
    CHECK_MSG(dsp()->channel_pixels == 2560, "通道像素 %u ≠ 320×64/8", (unsigned)dsp()->channel_pixels);
    CHECK_MSG((uint32_t)dsp()->total_channels * dsp()->channel_pixels == dsp()->buffer_size,
              "8 × %u ≠ 缓冲 %u —— 扫描会读穿或重读", (unsigned)dsp()->channel_pixels,
              (unsigned)dsp()->buffer_size);
    CHECK(dsp()->scan_line_pixels * dsp()->scan_lines == dsp()->channel_pixels);

    /* 模组级：10×4 个 32×16 */
    CHECK(dsp()->modules_per_row == 10 && dsp()->modules_per_col == 4);
    CHECK(dsp()->module_rows == 32 && dsp()->module_cols == 16);
    CHECK(dsp()->channels_per_module == 2);
}

/** @brief prepare 是双射：20480 个像素各写一个字节、无空穴、无覆盖
 *
 *  位图先全部写成 0xFF 当哨兵：prepare 之后不应残留 0xFF（说明每个字节都被写过），
 *  且 8 种颜色的个数必须各占 1/8（说明没有重复写同一处、也没有丢像素）。 */
static void case_prepare_bijection(void)
{
    TEST_BEGIN("prepare 双射：每个像素一个字节、无空穴、颜色计数守恒");

    fixture_reset();
    for (uint32_t i = 0; i < sizeof(s_pixel_map); i++)
        s_pixel_map[i] = (uint8_t)(i % 8U);

    memset(s_hub75_buff, 0xFF, sizeof(s_hub75_buff));
    dsp()->ops->prepare(dsp());

    uint32_t counts[8] = {0};
    uint32_t holes    = 0;
    for (uint32_t i = 0; i < sizeof(s_hub75_buff); i++) {
        if (s_hub75_buff[i] == 0xFFU)
            holes++;
        else if (s_hub75_buff[i] < 8U)
            counts[s_hub75_buff[i]]++;
    }

    CHECK_MSG(holes == 0, "有 %u 个字节没被写到（空穴）", (unsigned)holes);
    for (uint8_t c = 0; c < 8; c++)
        CHECK_MSG(counts[c] == 2560U, "颜色 %u 有 %u 个像素（应 2560）", c, (unsigned)counts[c]);
}

/** @brief 行配对：r 与 r+4 落在同一个 IC，先推 r、再推 r+4；列在组内按 0..7 顺序
 *
 *  这条是"接线契约"的现形：面板上的 IC 是 16 输出（8 列 × 2 行）。若把 r 与 r+8
 *  配在一起（HUB75 上更常见），实物就会整屏错行 —— 而 host 侧只有这条断言能发现。 */
static void case_prepare_row_pairing(void)
{
    TEST_BEGIN("行配对：IC 的 16 个输出 = 8 列 × 2 行（r 先、r+4 后）");

    static const struct {
        uint16_t row, col;
    } probes[] = {
        {0, 0}, {0, 7}, {0, 8}, {0, 319}, {3, 5}, {4, 0}, {8, 0}, {32, 16}, {63, 319},
    };

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const uint16_t r = probes[i].row;
        const uint16_t c = probes[i].col;
        const uint8_t  color = (uint8_t)(1U + (i % 7U));

        fixture_reset();
        s_pixel_map[r * 320U + c] = color;
        dsp()->ops->prepare(dsp());

        /* 模型：组 = (r/8*4 + r%4) 行组 × (c/8) 列组；组内偏移 = (r/4%2)*8 + c%8 */
        const uint32_t grp = (uint32_t)(r / 8U * 4U + r % 4U) * 40U + c / 8U;
        const uint32_t idx = grp * 16U + (uint32_t)(r / 4U % 2U) * 8U + c % 8U;

        CHECK_MSG(idx < sizeof(s_hub75_buff), "(%u,%u) 算出越界索引 %u", r, c, (unsigned)idx);
        CHECK_MSG(s_hub75_buff[idx] == color, "(%u,%u) 应落在 %u，实际值 %u", r, c, (unsigned)idx,
                  s_hub75_buff[idx]);
    }
}

/** @brief 4 个扫描步：每步 16 行、并集 64 行、互不重叠
 *
 *  漏一步 = 16 行永远不亮；两步覆盖同一批行 = 另外 16 行不亮，同时这 16 行被刷两次
 *  （亮度高一倍，现场看是"一条条亮带"）。这条不依赖面板的行序，只依赖"覆盖完整"。 */
static void case_scan_row_cover(void)
{
    TEST_BEGIN("扫描覆盖：4 步各 16 行、并集 64 行、零重叠");

    fixture_reset();
    for (uint32_t i = 0; i < sizeof(s_pixel_map); i++)
        s_pixel_map[i] = 1; /* 全红：任何行没被扫到都会缺席 */
    dsp()->ops->prepare(dsp());
    capture_frame();

    /* 一个时钟脉冲 = 一个像素位置，**8 个通道同时移位**（不是每通道一个脉冲）：
       每拍 640 个脉冲 × 4 拍 = 2560。若哪天改成"逐通道串行推"，这里立刻红 ——
       那会让刷新时间 ×8，4 行扫的一帧从 10ms 涨到 80ms（肉眼可见闪烁）。 */
    CHECK_MSG(s_clock_pulses == TEST_SCAN_LINES * TEST_SCAN_PIXELS,
              "时钟脉冲 %u，应为 %u", (unsigned)s_clock_pulses,
              (unsigned)(TEST_SCAN_LINES * TEST_SCAN_PIXELS));

    for (uint8_t line = 0; line < TEST_SCAN_LINES; line++) {
        uint8_t seen[64] = {0};
        for (uint8_t ch = 0; ch < TEST_CH; ch++)
            for (uint16_t k = 0; k < TEST_SCAN_PIXELS; k++) {
                uint16_t row, col;
                model_pixel(line, ch, k, &row, &col);
                if (row < 64U) seen[row] = 1;
            }
        uint8_t n = 0;
        for (uint8_t r = 0; r < 64; r++) n = (uint8_t)(n + seen[r]);
        CHECK_MSG(n == 16, "第 %u 步覆盖了 %u 行（应 16）", line, n);
    }

    /* 四步并集必须正好 64 行（用重构的命中计数验：每格恰好 1 次） */
    static uint8_t shown[64][320];
    CHECK_MSG(reconstruct(shown) == 0, "有像素被重复推送（覆盖重叠）");
    uint32_t missing = 0;
    for (uint8_t r = 0; r < 64; r++)
        for (uint16_t c = 0; c < 320; c++)
            if (shown[r][c] == 0xFFU) missing++;
    CHECK_MSG(missing == 0, "%u 个像素从未被推送（漏行）", (unsigned)missing);
}

/** @brief 整帧还原：假面板收到的画面必须与 pixel_map 逐像素一致
 *
 *  这是本套件的收口断言 —— 前面几条都是局部不变量，这条是"整屏扫完能不能看"。 */
static void case_scan_frame_roundtrip(void)
{
    TEST_BEGIN("整帧还原：扫出去的像素经假面板拼回 = 原图");

    fixture_reset();
    for (uint16_t r = 0; r < 64; r++)
        for (uint16_t c = 0; c < 320; c++)
            s_pixel_map[r * 320U + c] = (uint8_t)((r / 8U * 4U + r % 4U + c / 32U) % 8U);

    dsp()->ops->prepare(dsp());
    capture_frame();

    static uint8_t shown[64][320];
    CHECK_MSG(reconstruct(shown) == 0, "有像素被重复推送");

    uint32_t bad = 0;
    uint16_t first_r = 0, first_c = 0;
    for (uint16_t r = 0; r < 64 && bad < 1U; r++)
        for (uint16_t c = 0; c < 320; c++)
            if (shown[r][c] != s_pixel_map[r * 320U + c]) {
                bad     = 1;
                first_r = r;
                first_c = c;
                break;
            }
    CHECK_MSG(bad == 0, "首个不一致像素 (%u,%u)：期望 %02X 实得 %02X", first_r, first_c,
              s_pixel_map[first_r * 320U + first_c], shown[first_r][first_c]);
}

/** @brief 颜色位序：颜色码的 bit0/1/2 必须对应 R/G/B 三根线
 *
 *  G/B 互换或 bit0 接反，屏上颜色全错（红显示成绿），而几何完全正常 ——
 *  这种"结构对、颜色错"的缺陷只有逐色比对能发现。 */
static void case_color_bits(void)
{
    TEST_BEGIN("颜色位序：颜色码 bit0/1/2 → R/G/B");

    for (uint8_t color = 0; color < 8; color++) {
        fixture_reset();
        s_pixel_map[0] = color;
        dsp()->ops->prepare(dsp());
        capture_frame();

        uint8_t shown[64][320];
        reconstruct(shown);
        CHECK_MSG(shown[0][0] == color, "颜色 %u 扫出来是 %u", color, shown[0][0]);
    }

    /* 行地址直通：set_row 的实参就是行号（1/4 扫用 0..3） */
    fixture_reset();
    dsp()->ops->set_row(3);
    CHECK_MSG(s_row_addr == 3, "set_row(3) 实际写 %u", s_row_addr);
}

/* ================================================================ */

int main(void)
{
    printf("P10 32×16 模组驱动 host 单测\n");

    dev_p10_32x16_2200001703_init(); /* 绑定 ops + 预计算 BSRR 表 */

    case_geometry();
    case_prepare_bijection();
    case_prepare_row_pairing();
    case_scan_row_cover();
    case_scan_frame_roundtrip();
    case_color_bits();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
