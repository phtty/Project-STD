/**
 * @file    test_screen_layout.c
 * @brief   切分表与抽带：画布上的一个矩形 → 一张 1bpp 位图
 *
 * **为什么需要这个测试**：抽带是级联的**唯一**内容出口 —— 主卡本地落屏与发给从卡
 * 走的是同一个 `app_screen_extract`。它错了的表现是"某张卡右边差一列"或"某一行
 * 整体错位"，而两块屏分别在两台设备上、靠人眼比对根本看不出来。这里用一份**独立
 * 的参考实现**（1 字节/像素的整屏帧缓冲 + 按矩形打包）把它钉成逐位相等。
 *
 * 参考实现走的是 1B/px 帧缓冲，与画布的 1bpp 位掩码是两套表示 —— 所以它能抓到
 * "画布自己写错了"以外的错（这正是 test_screen_canvas.c 覆盖的那部分），
 * 两者互补而非重复。
 *
 * ---- 网格三个方向都要测 ----
 * 切分表是 COLS×ROWS 网格：COLS>1 是**左右**拼，ROWS>1 是**上下**堆叠。
 * **只测 2×1 会漏掉抽带的 y 偏移**（纯横向时每张卡的矩形 y 恒为 0，漏加 y 偏移
 * 也看不出来），而上下拼法的现场表现正是"上半屏的内容跑到下半屏去"。
 * 所以本文件对 2×1 / 1×2 / 2×2 三种网格各跑一遍同一组断言。
 *
 * ---- 本用例为什么把本卡设成**第二张** ----
 * `BOARD_CASCADE_ADDR=1`：矩形不在画布原点。原点那张卡的矩形与"整块画布"重合，
 * 抽带就算整个写错（比如忘了加矩形偏移）也看不出来。第二张卡同时覆盖
 * "矩形有偏移"和"末字节补位"两件事。
 *
 * 单卡 1×1 的退化路径不在这里测 —— 那要求本卡地址是 0，而本文件的地址被钉成 1
 * （见上）。那条由 test_screen_canvas.c 覆盖（它是默认的 1×1 网格 + addr 0）。
 */

#define BOARD_SCREEN_CANVAS 1
#define BOARD_CASCADE_ADDR  1 /* 本卡 = 第二张（网格下标 1，非原点矩形） */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h" /* pl_task_new 的桩要用到线程类型 */
#include "dev_display.h"
#include "app_render.h"
#include "app_screen.h"
#include "board.h"

/* ---- app_screen.c 依赖、本用例不关心的接口 ---- */
dev_display_t *dev_display_get(void); /* 定义见下方夹具之后 */
osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    return nullptr;
}
void app_render_set_target(const render_target_t *t) { (void)t; }
void app_render_set_persist_hook(const render_persist_hook_t *h) { (void)h; }
void app_render_save(void) {}
bool app_render_restore(void) { return false; }

/* ---- dev_display 原语：与 test_screen_canvas.c 同一份参考实现 ---- */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, display_color_t color)
{
    if (x < dev->screen_rows && y < dev->screen_cols) {
        dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
        dev->dirty                                = true;
    }
}
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      display_color_t color)
{
    if (x + w > dev->screen_rows) w = dev->screen_rows - x;
    if (y + h > dev->screen_cols) h = dev->screen_cols - y;
    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    dev->dirty = true;
}
void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, display_color_t color)
{
    if (x + w > dev->screen_rows || y + h > dev->screen_cols) return;
    uint16_t row_bytes = (w + 7) / 8;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            if (bitmap[row * row_bytes + col / 8] & (0x80 >> (col % 8)))
                dev->pixel_map[(y + row) * dev->screen_rows + (x + col)] = (uint8_t)color;
    dev->dirty = true;
}
void dev_display_set_brightness(dev_display_t *dev, uint8_t level)
{
    if (level > 7) level = 7;
    dev->light_level = level;
}

/* ---- 被测：生产源码本体 ---- */
#include "../Application/Src/app_screen.c"

/* ================================================================
 *  夹具
 * ================================================================ */

/* 整屏上限：最多 2×2 张 48×16 的卡 → 96×32 */
#define FB_MAX_W (96U)
#define FB_MAX_H (32U)

/** 单卡位图缓冲：本用例最大的一块是 48×16 → 6×16 = 96 字节，留到 128 */
#define BM_MAX (128U)

static dev_display_t s_dev;
static uint8_t       s_fb[FB_MAX_W * FB_MAX_H]; /* 本卡实屏（1B/px）—— 只有**本卡**那么大 */
static uint16_t      s_w;
static uint16_t      s_h;

/* 参考用的**整屏**帧缓冲（1B/px）。
   **必须与 s_fb 分开**：实屏只是整屏里本卡那一块（多卡时比整屏窄），拿实屏当参考
   去打包"别的卡那一块"会读到屏外 —— 本用例第一版就是这么错的，ASan 当场抓到。 */
static uint8_t  s_ref[FB_MAX_W * FB_MAX_H];
static uint16_t s_ref_w; /* 整屏宽 */
static uint16_t s_ref_h; /* 整屏高 */

static void display_reset(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    memset(s_fb, COLOR_BLACK, sizeof(s_fb));
    s_dev.screen_rows = w;
    s_dev.screen_cols = h;
    s_dev.pixel_map   = s_fb;
    s_dev.light_level = 7;
    s_dev.dirty       = false;
}

dev_display_t *dev_display_get(void)
{
    return &s_dev;
}

/** @brief 按给定几何与网格重建切分表与画布
 *
 *  走的是生产里的两个真函数（`_layout_build_grid` + `_apply_layout`）——
 *  `_screen_init` 只是它们的调用者外加"注册渲染目标 / 建任务"，那两步在 host 上
 *  没有意义（本文件已把 app_render 与 pl_task 桩掉）。 */
static void canvas_reset_mc(uint16_t w, uint16_t h, uint8_t nx, uint8_t ny, uint8_t master_cell)
{
    if ((uint32_t)w * nx > FB_MAX_W || (uint32_t)h * ny > FB_MAX_H) {
        printf("      夹具失败：整屏 %ux%u 超出本用例缓冲\n", (unsigned)(w * nx),
               (unsigned)(h * ny));
        return;
    }
    display_reset(w, h);
    memset(s_ref, COLOR_BLACK, sizeof(s_ref));
    s_ref_w   = (uint16_t)(w * nx);
    s_ref_h   = (uint16_t)(h * ny);
    s_display = &s_dev; /* _screen_init 的第一件事 */
    if (!_layout_build_grid(nx, ny, master_cell)) {
        printf("      夹具失败：切分表 %ux%u（主卡格 %u）没合成出来\n", nx, ny, master_cell);
        return;
    }
    if (!_apply_layout()) {
        printf("      夹具失败：本卡 addr=%u 不在 %ux%u 的切分表里\n", (unsigned)BOARD_CASCADE_ADDR,
               nx, ny);
    }
}

/** 默认主卡在格 0（左上）—— 绝大多数用例只关心几何，不关心谁主谁从 */
static void canvas_reset(uint16_t w, uint16_t h, uint8_t nx, uint8_t ny)
{
    canvas_reset_mc(w, h, nx, ny, 0);
}

/* ---- 独立参考实现（1B/px 帧缓冲） ---- */

/** @brief 参考填充：直接写**整屏**帧缓冲（坐标系是整屏，不是本卡） */
static void ref_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t c)
{
    for (uint16_t r = 0; r < h; r++)
        for (uint16_t k = 0; k < w; k++)
            if (x + k < s_ref_w && y + r < s_ref_h) s_ref[(y + r) * s_ref_w + (x + k)] = (uint8_t)c;
}

/** @brief 参考打包：帧缓冲里的一块矩形 → 1bpp（(宽+7)/8 行字节、MSB-first、黑=0）
 *
 *  末字节补位天然为 0（只置矩形内的位）—— 这正是待测实现要与之相等的约定。 */
static void pack_rect_ref(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh, uint8_t *out)
{
    uint16_t stride = (uint16_t)((rw + 7) / 8);
    memset(out, 0, stride * rh);
    for (uint16_t y = 0; y < rh; y++)
        for (uint16_t x = 0; x < rw; x++)
            if (s_ref[(uint32_t)(ry + y) * s_ref_w + (rx + x)] != COLOR_BLACK)
                out[(uint32_t)y * stride + (x >> 3)] |= (uint8_t)(0x80U >> (x & 7));
}

/** @brief 同一条图案同时画进画布（生产路径）与参考帧缓冲（参考路径）
 *
 *  图案刻意让**每条卡缝都被跨过**：
 *   · 每 3 列一根**通高**竖条 —— 竖条跨越水平卡缝（上下拼法），也抓列索引错位
 *   · 一段横条跨过竖直卡缝
 *   · 一段竖条跨过水平卡缝
 *  只有图案真的跨过缝，漏加 x/y 偏移才会露出来。 */
static void paint_pattern(void)
{
    for (uint16_t x = 0; x < s_ref_w; x += 3) {
        _sink_fill(nullptr, x, 0, 1, s_ref_h, COLOR_RED);
        ref_fill(x, 0, 1, s_ref_h, COLOR_RED);
    }
    const uint16_t cx = (uint16_t)(s_ref_w / 2);
    const uint16_t cy = (uint16_t)(s_ref_h / 2);

    if (cx >= 5) { /* 横条跨竖直卡缝 */
        _sink_fill(nullptr, (uint16_t)(cx - 5), cy, 10, 3, COLOR_GREEN);
        ref_fill((uint16_t)(cx - 5), cy, 10, 3, COLOR_GREEN);
    }
    if (cy >= 5) { /* 竖条跨水平卡缝 */
        _sink_fill(nullptr, cx, (uint16_t)(cy - 5), 3, 10, COLOR_BLUE);
        ref_fill(cx, (uint16_t)(cy - 5), 3, 10, COLOR_BLUE);
    }
}

/* ---- 断言 ---- */
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

static void check_bitmaps_equal(const uint8_t *a, const uint8_t *b, uint16_t len, const char *what)
{
    for (uint16_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            CHECK_MSG(0, "%s 第 %u 字节不同：抽带 %02X / 参考 %02X", what, (unsigned)i, a[i], b[i]);
            return;
        }
    }
    CHECK_MSG(1, "%s", what);
}

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 逐卡与参考比对 + 矩形互不重叠。任意 nx×ny 网格通用 ——
 *         三种网格跑的是**同一组**断言，所以"上下拼法漏了 y 偏移"跑不掉。 */
static void check_all_cards(const char *what)
{
    const screen_layout_t *L = app_screen_layout();

    static uint8_t owner[FB_MAX_H][FB_MAX_W]; /* 位 i = 第 i 张卡碰过这个像素 */
    memset(owner, 0, sizeof(owner));

    for (uint8_t i = 0; i < L->count; i++) {
        const screen_card_t *c = &L->cards[i];
        const uint16_t       n = app_screen_card_bm_len(i);
        uint8_t              got[BM_MAX], ref[BM_MAX];

        if (n > sizeof(got)) {
            CHECK_MSG(0, "%s：%u 号卡位图 %u 超出本用例缓冲", what, (unsigned)i, (unsigned)n);
            continue;
        }

        pack_rect_ref(c->x, c->y, c->w, c->h, ref);
        if (!app_screen_extract(i, got, sizeof(got))) {
            CHECK_MSG(0, "%s：%u 号卡抽带失败", what, (unsigned)i);
            continue;
        }
        char label[48];
        snprintf(label, sizeof(label), "%s · %u 号卡 %ux%u@(%u,%u)", what, (unsigned)i,
                 (unsigned)c->w, (unsigned)c->h, (unsigned)c->x, (unsigned)c->y);
        check_bitmaps_equal(got, ref, n, label);

        const uint16_t stride = (uint16_t)((c->w + 7) / 8);
        for (uint16_t y = 0; y < c->h; y++)
            for (uint16_t x = 0; x < c->w; x++)
                if (got[(uint32_t)y * stride + (x >> 3)] & (uint8_t)(0x80U >> (x & 7U)))
                    owner[c->y + y][c->x + x] |= (uint8_t)(1U << i);
    }

    /* 矩形不许重叠：同一像素被两张卡都点亮 = 矩形算错。
       （"有没有漏像素"由上面逐卡与参考比对覆盖 —— 漏掉的那块在两张卡里都不出现，
        只有参考实现知道它本该亮。） */
    uint16_t bad_x = 0xFFFF, bad_y = 0xFFFF;
    for (uint16_t y = 0; y < s_ref_h && bad_x == 0xFFFF; y++)
        for (uint16_t x = 0; x < s_ref_w; x++)
            if (owner[y][x] & (uint8_t)(owner[y][x] - 1U)) {
                bad_x = x;
                bad_y = y;
                break;
            }
    CHECK_MSG(bad_x == 0xFFFF, "%s：(%u,%u) 同时出现在两张卡的位图里（矩形重叠）", what,
              (unsigned)bad_x, (unsigned)bad_y);
}

/** 网格合成出来的几何与地址（三种拼法各验一次地址分配与矩形位置） */
static void case_grid_shape(void)
{
    TEST_BEGIN("切分表：左右 / 上下 / 四宫格的几何与地址");

    struct {
        const char *what;
        uint8_t     nx, ny;
    } grids[] = {
        {"2×1 左右", 2, 1},
        {"1×2 上下", 1, 2},
        {"2×2 四宫格", 2, 2},
    };

    for (unsigned g = 0; g < sizeof(grids) / sizeof(grids[0]); g++) {
        const uint8_t nx = grids[g].nx, ny = grids[g].ny;
        canvas_reset(48, 16, nx, ny);
        const screen_layout_t *L = app_screen_layout();

        CHECK_MSG(L->count == (uint8_t)(nx * ny), "%s：卡数应为 %u，得到 %u", grids[g].what,
                  (unsigned)(nx * ny), (unsigned)L->count);
        CHECK_MSG(L->rows == 48 * nx && L->cols == 16 * ny, "%s：整屏应为 %ux%u，得到 %ux%u",
                  grids[g].what, (unsigned)(48 * nx), (unsigned)(16 * ny), (unsigned)L->rows,
                  (unsigned)L->cols);

        /* 地址 = 网格下标（**行优先**）：addr = r*nx + c，矩形 (c*48, r*16) */
        bool shapes_ok = true;
        for (uint8_t r = 0; r < ny && shapes_ok; r++)
            for (uint8_t c = 0; c < nx; c++) {
                const screen_card_t *k = &L->cards[(uint8_t)(r * nx + c)];
                if (k->addr != (uint8_t)(r * nx + c) || k->x != (uint16_t)(c * 48) ||
                    k->y != (uint16_t)(r * 16) || k->w != 48 || k->h != 16) {
                    CHECK_MSG(0, "%s：%u 行 %u 列的卡应为 addr=%u 矩形 48x16@(%u,%u)", grids[g].what,
                              (unsigned)r, (unsigned)c, (unsigned)(r * nx + c), (unsigned)(c * 48),
                              (unsigned)(r * 16));
                    shapes_ok = false;
                    break;
                }
            }
        if (shapes_ok) CHECK_MSG(1, "%s：地址与矩形位置", grids[g].what);

        /* 本用例的本卡是 addr=1 → 恒为网格下标 1（0 行 1 列） */
        CHECK_MSG(app_screen_self_index() == 1, "%s：本卡（addr=1）应落在下标 1，得到 %u",
                  grids[g].what, (unsigned)app_screen_self_index());
        CHECK_MSG(s_rows == L->rows && s_cols == L->cols && s_stride == (uint16_t)((L->rows + 7) / 8),
                  "%s：画布几何应与整屏一致", grids[g].what);
    }

    /* 越界下标 */
    canvas_reset(48, 16, 2, 1);
    CHECK_MSG(app_screen_card_bm_len(2) == 0, "越界下标应返回 0");
    CHECK_MSG(app_screen_card_bm_len(1) <= BOARD_CASCADE_BAND_MAX,
              "本卡位图 %u 超过 BOARD_CASCADE_BAND_MAX %u", (unsigned)app_screen_card_bm_len(1),
              (unsigned)BOARD_CASCADE_BAND_MAX);
}

/** 三种网格下逐卡抽带都等于参考（含跨缝图案） */
static void case_extract_grids(void)
{
    TEST_BEGIN("抽带：左右 / 上下 / 四宫格，逐卡等于参考打包");

    struct {
        const char *what;
        uint8_t     nx, ny;
    } grids[] = {
        {"2×1 左右", 2, 1},
        {"1×2 上下", 1, 2},
        {"2×2 四宫格", 2, 2},
    };

    for (unsigned g = 0; g < sizeof(grids) / sizeof(grids[0]); g++) {
        canvas_reset(48, 16, grids[g].nx, grids[g].ny);
        paint_pattern();
        check_all_cards(grids[g].what);
    }
}

/** 非对齐矩形 + 末字节补位：卡宽 44（44%8=4），x 也不是 8 的倍数 */
static void case_extract_unaligned(void)
{
    TEST_BEGIN("抽带：x%8≠0 且 w%8≠0 的矩形（末字节补位必须归零）");

    canvas_reset(44, 16, 2, 1); /* 整屏 88x16；1 号卡在 x=44 */
    paint_pattern();
    check_all_cards("44 宽 2×1");

    /* 显式钉住补位：1 号卡矩形是 x=44..87，末字节覆盖 x=84..87 这 4 个真实位 +
       高 4 位补位。 */
    canvas_reset(44, 16, 2, 1);
    _sink_fill(nullptr, 84, 0, 4, 1, COLOR_RED); /* 1 号卡的真实位 84..87 */
    uint8_t        bm[BM_MAX];
    const uint16_t stride = 6; /* ceil(44/8) */
    CHECK_MSG(app_screen_extract(1, bm, sizeof(bm)), "1 号卡抽带应成功");
    CHECK_MSG((bm[stride - 1] & 0x0FU) == 0U, "末字节高 4 位是补位，必须为 0，得到 %02X",
              bm[stride - 1]);
    CHECK_MSG((bm[stride - 1] & 0xF0U) == 0xF0U, "末字节低 4 位是真实像素（应全亮），得到 %02X",
              bm[stride - 1]);

    /* 上下拼法 + 非对齐**行**：卡高 12（12%…高度不参与位打包，但要保证 y 偏移对） */
    canvas_reset(44, 12, 1, 2); /* 整屏 44x24，1 号卡在 y=12 */
    paint_pattern();
    check_all_cards("44×12 上下");
}

/** 越界与容量：一律拒绝，且不得动调用方的缓冲 */
static void case_extract_bounds(void)
{
    TEST_BEGIN("抽带：越界下标/容量不足一律拒绝，不动 buf");

    canvas_reset(48, 16, 2, 1);

    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));

    CHECK_MSG(!app_screen_extract(2, buf, sizeof(buf)), "下标 == count 应拒绝");
    CHECK_MSG(!app_screen_extract(0, buf, 0), "容量 0 应拒绝");
    CHECK_MSG(!app_screen_extract(0, buf, (uint16_t)(app_screen_card_bm_len(0) - 1)),
              "容量少一字节应拒绝");
    CHECK_MSG(!app_screen_extract(0, nullptr, sizeof(buf)), "buf 为空应拒绝");

    bool untouched = true;
    for (uint32_t i = 0; i < sizeof(buf); i++)
        if (buf[i] != 0xAA) {
            untouched = false;
            break;
        }
    CHECK_MSG(untouched, "被拒绝的调用改动了调用方的缓冲");
}

/** 本地落屏与抽带必须是同一份内容（主从两侧同步的前提） */
static void case_commit_self_matches_canvas(void)
{
    TEST_BEGIN("本卡落屏：实屏内容 == 本卡矩形在画布上的内容");

    canvas_reset(44, 12, 1, 2); /* 本卡 = 1 号卡，矩形 44x12@(0,12)：y 偏移非 0 */
    paint_pattern();

    CHECK_MSG(app_screen_commit_self(), "本卡落屏应成功");

    const screen_card_t *c = &app_screen_layout()->cards[app_screen_self_index()];
    uint8_t              bm[BM_MAX];
    CHECK_MSG(app_screen_extract(app_screen_self_index(), bm, sizeof(bm)), "抽带应成功");

    /* 逐像素比对：实屏（1B/px，本卡尺寸）× 画布（1bpp）里本卡那块矩形 */
    uint16_t diff  = 0;
    uint16_t stride = (uint16_t)((c->w + 7) / 8);
    for (uint16_t y = 0; y < s_h; y++)
        for (uint16_t x = 0; x < s_w; x++) {
            bool on_screen = (s_fb[(uint32_t)y * s_w + x] != COLOR_BLACK);
            bool on_canvas = (bm[(uint32_t)y * stride + (x >> 3)] & (uint8_t)(0x80U >> (x & 7U))) != 0;
            if (on_screen != on_canvas) diff++;
        }
    CHECK_MSG(diff == 0, "实屏与画布有 %u 个像素不一致", (unsigned)diff);

    /* 落屏用的颜色必须是**本卡**的（切分表逐卡给），不是全局默认 */
    CHECK_MSG(s_fb[0] == COLOR_BLACK || s_fb[0] == c->color, "落屏颜色不是本卡那一色");
}

/** 主卡在**下面**那一块 —— 现场的实际拼法。地址不按网格下标编。 */
static void case_master_below(void)
{
    TEST_BEGIN("主卡在下方：地址按「主卡那格编 0」分配，抽带仍按矩形");

    /* 1×2 网格：格 0 在上、格 1 在下；主卡是**下面那块** → 格 1 编 addr 0 */
    canvas_reset_mc(48, 16, 1, 2, 1);
    const screen_layout_t *L = app_screen_layout();

    CHECK_MSG(L->count == 2 && L->rows == 48 && L->cols == 32, "整屏应为 48x32，得到 %ux%u",
              (unsigned)L->rows, (unsigned)L->cols);
    CHECK_MSG(L->cards[0].addr == 1, "上面的卡应编 addr 1（主卡在下），得到 %u",
              (unsigned)L->cards[0].addr);
    CHECK_MSG(L->cards[1].addr == 0, "下面的卡应编 addr 0（主卡），得到 %u",
              (unsigned)L->cards[1].addr);
    CHECK_MSG(L->cards[0].y == 0 && L->cards[1].y == 16,
              "矩形仍按几何排：上半 y=0 / 下半 y=16，得到 %u / %u", (unsigned)L->cards[0].y,
              (unsigned)L->cards[1].y);

    /* 本用例的本卡是 addr=1 → 就是**上面**那块（主卡在下面） */
    CHECK_MSG(app_screen_self_index() == 0, "本卡（addr=1）应落在下标 0（上半屏），得到 %u",
              (unsigned)app_screen_self_index());

    paint_pattern();
    check_all_cards("1×2 主卡在下");
}

/** 地址 ↔ 下标 ↔ 矩形 的对应必须钉死 —— 级联最容易写错、且错了不报错的地方
 *
 *  协议按**地址**寻址，切分表按**下标**索引，两者顺序可以相反：主卡在下时
 *  （master_cell=1）下标 0 是 addr 1（上面那块）、下标 1 是 addr 0（下面那块）。
 *  谁要是把地址当下标去索引，就会把主卡的矩形发给从卡、从卡的发给主卡 ——
 *  两块屏内容**互换**，而 CRC / 长度 / 几何校验**全部通过**，没有任何一处报错。 */
static void case_addr_index_mapping(void)
{
    TEST_BEGIN("地址 ↔ 下标 ↔ 矩形：主卡在下时两者顺序相反，不许拿地址当下标");

    canvas_reset_mc(48, 16, 1, 2, 1); /* 1×2，主卡在格 1（下面那块） */

    CHECK_MSG(app_screen_index_of_addr(0) == 1, "addr 0（主卡）应落在下标 1，得到 %u",
              (unsigned)app_screen_index_of_addr(0));
    CHECK_MSG(app_screen_index_of_addr(1) == 0, "addr 1 应落在下标 0，得到 %u",
              (unsigned)app_screen_index_of_addr(1));

    const screen_card_t *bottom = app_screen_card(app_screen_index_of_addr(0)); /* 正确用法 */
    const screen_card_t *oops   = app_screen_card(0);                          /* 地址当下标 */

    CHECK_MSG(bottom && bottom->addr == 0 && bottom->y == 16,
              "addr 0 的矩形应在下半屏（addr=0, y=16），得到 addr=%u y=%u",
              bottom ? (unsigned)bottom->addr : 999U, bottom ? (unsigned)bottom->y : 999U);

    /* 这条是**自检**：如果哪天网格/地址分配变了、两种取法恰好取到同一项，
       上面那些断言就失去了分辨力，这条会先把这件事说出来。 */
    CHECK_MSG(bottom && oops && bottom != oops && bottom->y != oops->y,
              "本用例必须让「地址当下标」取到**另一块**矩形，否则这里测不出东西");

    CHECK_MSG(app_screen_card(2) == nullptr, "越界下标应返回 nullptr");
    CHECK_MSG(app_screen_card_bm_len(2) == 0, "越界下标的位图长度应为 0");
    CHECK_MSG(app_screen_index_of_addr(9) == 0xFF, "表中没有的地址应返回 0xFF");
}

/** 地址不在切分表里：停用门面，**不静默降级**成"单卡占满" */
static void case_self_addr_not_in_table(void)
{
    TEST_BEGIN("本卡地址不在切分表里 → 停用，不降级");

    display_reset(48, 16);
    s_display = &s_dev;

    /* 板级网格是 1×1（只有 addr=0 一张卡），而本用例的本卡是 addr=1 */
    _screen_init();

    CHECK_MSG(s_display == nullptr, "地址不在表里时整屏门面应停用，而不是自己占满整屏");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m切分表与抽带（本卡 addr=%u）\033[0m\n", (unsigned)BOARD_CASCADE_ADDR);

    case_grid_shape();
    case_extract_grids();
    case_extract_unaligned();
    case_extract_bounds();
    case_commit_self_matches_canvas();
    case_master_below();
    case_addr_index_mapping();
    case_self_addr_not_in_table();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
