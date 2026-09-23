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
 * `BOARD_CASC_ADDR=1`：矩形不在画布原点。原点那张卡的矩形与"整块画布"重合，
 * 抽带就算整个写错（比如忘了加矩形偏移）也看不出来。第二张卡同时覆盖
 * "矩形有偏移"和"末字节补位"两件事。
 *
 * 单卡 1×1 的退化路径不在这里测 —— 那要求本卡地址是 0，而本文件的地址被钉成 1
 * （见上）。那条由 test_screen_canvas.c 覆盖（它是默认的 1×1 网格 + addr 0）。
 */

#define BOARD_SCREEN_CANVAS 1
#define BOARD_CASC_ADDR  1 /* 本卡 = 第二张（网格下标 1，非原点矩形） */

/* **钉住切分参数，不跟着 board.h 的现场配置变**：本用例用 _layout_build_grid()
   显式指定网格，但 _screen_init() / 换身份重装走的是 board.h 的部署配置 —— 那是
   **现场**参数（可能是 1×2、主卡在下）。跟着它变的话，同一份测试在不同板子上测出
   不同结论，而且不报错，只是静默地少测几条。

   钉成 **1×2**（不是 1×1）是为了让"主卡(格 0)/从卡(格 1)"**两种身份都在表里**：
   换身份时"按新身份装卸钩子"的两条路都要走一遍，1×1 的话从卡只能落到"地址不在表里、
   门面停用"那条路上，`_apply_identity` 里给从卡留的那一支就一条也测不到。 */
#define BOARD_CASC_COLS        1
#define BOARD_CASC_ROWS        2
#define BOARD_CASC_MASTER_CELL 0

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
/* 渲染目标 / 持久化钩子的装卸要能被看见：换身份的两条路（从主变从、从从变主）
   都是靠这两个调用完成的，而**漏掉任何一边**在屏上都不报错（只是不落屏 / 画错地方）。 */
static const app_render_target_t       *s_target_last;
static const app_render_persist_hook_fn_t *s_hook_last;
static int                          s_target_calls;

void app_render_set_target(const app_render_target_t *t)
{
    s_target_last = t;
    s_target_calls++;
}
void app_render_set_persist_hook(const app_render_persist_hook_fn_t *h) { s_hook_last = h; }
/* 落盘计数：本套件要断言"**落屏之后**才存"（存早了存的就是上一帧） */
static int  s_save_calls;
void        app_render_save(void) { s_save_calls++; }
/* 恢复桩的状态：用例可让它"恢复"出指定颜色的一块内容（持久化颜色那条用例要用）。
   函数体在 `s_dev` 声明之后（要往实屏上画）。 */
static bool            s_restore_ok;
static dev_display_color_t s_restore_color;
bool app_render_restore(void);
/* "这一帧要落盘"的请求位：生产里由 app_render 置，本套件直接摆它 */
static bool s_persist_req;
bool app_render_take_persist_req(void)
{
    const bool r  = s_persist_req;
    s_persist_req = false;
    return r;
}
bool app_render_peek_persist_req(void) { return s_persist_req; }

/* ---- dev_display 原语：与 test_screen_canvas.c 同一份参考实现 ---- */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color)
{
    if (x < dev->screen_rows && y < dev->screen_cols) {
        dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
        dev->dirty                                = true;
    }
}
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      dev_display_color_t color)
{
    if (x + w > dev->screen_rows) w = dev->screen_rows - x;
    if (y + h > dev->screen_cols) h = dev->screen_cols - y;
    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    dev->dirty = true;
}
void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, dev_display_color_t color)
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
/* ---- 多步绘制当成一帧：与生产同语义（test_screen_* 都 include app_screen.c）---- */
void dev_display_frame_begin(dev_display_t *dev)
{
    if (!dev) return;
    dev->dirty_hold    = true;
    dev->frame_touched = false;
}
void dev_display_frame_end(dev_display_t *dev)
{
    if (!dev) return;
    dev->dirty_hold = false;
    if (dev->frame_touched) {
        dev->dirty         = true;
        dev->frame_touched = false;
    }
}


/* ---- 被测：生产源码本体 ----
   画布实现已拆到独立 TU（app_screen_canvas.c），用例同样 TU-include ——
   否则 static sink / 画布缓冲与接缝符号都缺失，链接报未定义。 */
#include "../Application/Src/Render/app_screen.c"
#include "../Application/Src/Render/app_screen_canvas.c"

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
    memset(s_fb, DEV_DISPLAY_COLOR_BLACK, sizeof(s_fb));
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

/** @brief 恢复桩：按"记录里的颜色"往实屏上画一块内容（模拟真 restore 的产物） */
bool app_render_restore(void)
{
    if (!s_restore_ok) return false;
    dev_display_fill(&s_dev, 0, 0, s_dev.screen_rows, s_dev.screen_cols, DEV_DISPLAY_COLOR_BLACK);
    dev_display_fill(&s_dev, 0, 0, 4, 4, s_restore_color);
    return true;
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
    memset(s_ref, DEV_DISPLAY_COLOR_BLACK, sizeof(s_ref));
    s_ref_w   = (uint16_t)(w * nx);
    s_ref_h   = (uint16_t)(h * ny);
    s_display_dev = &s_dev; /* _screen_init 的第一件事 */
    if (!_layout_build_grid(nx, ny, master_cell)) {
        printf("      夹具失败：切分表 %ux%u（主卡格 %u）没合成出来\n", nx, ny, master_cell);
        return;
    }
    if (!_apply_layout()) {
        printf("      夹具失败：本卡 addr=%u 不在 %ux%u 的切分表里\n", (unsigned)BOARD_CASC_ADDR,
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
static void ref_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t c)
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
            if (s_ref[(uint32_t)(ry + y) * s_ref_w + (rx + x)] != DEV_DISPLAY_COLOR_BLACK)
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
        _sink_fill(nullptr, x, 0, 1, s_ref_h, DEV_DISPLAY_COLOR_RED);
        ref_fill(x, 0, 1, s_ref_h, DEV_DISPLAY_COLOR_RED);
    }
    const uint16_t cx = (uint16_t)(s_ref_w / 2);
    const uint16_t cy = (uint16_t)(s_ref_h / 2);

    if (cx >= 5) { /* 横条跨竖直卡缝 */
        _sink_fill(nullptr, (uint16_t)(cx - 5), cy, 10, 3, DEV_DISPLAY_COLOR_GREEN);
        ref_fill((uint16_t)(cx - 5), cy, 10, 3, DEV_DISPLAY_COLOR_GREEN);
    }
    if (cy >= 5) { /* 竖条跨水平卡缝 */
        _sink_fill(nullptr, cx, (uint16_t)(cy - 5), 3, 10, DEV_DISPLAY_COLOR_BLUE);
        ref_fill(cx, (uint16_t)(cy - 5), 3, 10, DEV_DISPLAY_COLOR_BLUE);
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
    const app_screen_layout_t *L = app_screen_layout();

    static uint8_t owner[FB_MAX_H][FB_MAX_W]; /* 位 i = 第 i 张卡碰过这个像素 */
    memset(owner, 0, sizeof(owner));

    for (uint8_t i = 0; i < L->count; i++) {
        const app_screen_card_t *c = &L->cards[i];
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
        const app_screen_layout_t *L = app_screen_layout();

        CHECK_MSG(L->count == (uint8_t)(nx * ny), "%s：卡数应为 %u，得到 %u", grids[g].what,
                  (unsigned)(nx * ny), (unsigned)L->count);
        CHECK_MSG(L->rows == 48 * nx && L->cols == 16 * ny, "%s：整屏应为 %ux%u，得到 %ux%u",
                  grids[g].what, (unsigned)(48 * nx), (unsigned)(16 * ny), (unsigned)L->rows,
                  (unsigned)L->cols);

        /* 地址 = 网格下标（**行优先**）：addr = r*nx + c，矩形 (c*48, r*16) */
        bool shapes_ok = true;
        for (uint8_t r = 0; r < ny && shapes_ok; r++)
            for (uint8_t c = 0; c < nx; c++) {
                const app_screen_card_t *k = &L->cards[(uint8_t)(r * nx + c)];
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
    CHECK_MSG(app_screen_card_bm_len(1) <= BOARD_CASC_BAND_MAX,
              "本卡位图 %u 超过 BOARD_CASC_BAND_MAX %u", (unsigned)app_screen_card_bm_len(1),
              (unsigned)BOARD_CASC_BAND_MAX);
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
    _sink_fill(nullptr, 84, 0, 4, 1, DEV_DISPLAY_COLOR_RED); /* 1 号卡的真实位 84..87 */
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
/** 持久化必须发生在**落屏之后** —— 这是"存到上一帧"那个缺陷的守门
 *
 *  原来是"渲染完立刻 `app_render_save()`"，而那时内容还在画布上、没落屏，
 *  `_persist_save` 读实屏存下去的是**上一帧**。改成"渲染只置请求位、落屏时才取走"。
 *
 *  **反向验证**：把 `commit_self` 里那句 `app_render_take_persist_req()` 挪回
 *  "渲染之后立刻存"（或在渲染路径上直接调 `app_render_save()`），本用例立刻红。 */
static void case_persist_after_commit(void)
{
    TEST_BEGIN("持久化发生在**落屏之后**（存这一帧，不是上一帧）");

    canvas_reset(44, 12, 1, 2); /* 与落屏用例同一布局：本卡 = 1 号卡 */
    paint_pattern();

    /* 模拟"上位机要求这次内容长期保留" */
    s_persist_req = true;
    s_save_calls  = 0;

    /* ① 只渲染（写画布）**不许**落盘：此刻实屏还是上一帧。
       8×8 的区域 = (8+7)/8 × 8 = 8 字节位图（每行 1 字节）—— 少给会越界读。 */
    static const uint8_t bm8[8] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};
    _sink_bitmap(&s_target, 0, 0, 8, 8, bm8, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(s_save_calls == 0, "只渲染就落盘了 —— 存下去的是上一帧（得到 %d 次）", s_save_calls);

    /* ② 画布闩此时应为真（级联靠它闸开轮：没渲染过就别把不全的画布推下去） */
    CHECK_MSG(app_screen_canvas_touched(), "渲染之后画布闩应置位");

    /* ③ 落屏 —— 这时才允许存，且恰好一次 */
    CHECK_MSG(app_screen_commit_self(), "本卡落屏应成功");
    CHECK_MSG(s_save_calls == 1, "落屏之后应恰好落盘一次，得到 %d", s_save_calls);

    /* ④ 请求位被取走（不许反复存） */
    CHECK_MSG(!app_render_peek_persist_req(), "请求位应已被取走");

    /* ⑤ 没有请求时落屏不该存 */
    CHECK_MSG(app_screen_commit_self(), "再落一次屏应成功");
    CHECK_MSG(s_save_calls == 1, "没有请求时不该落盘，得到 %d 次", s_save_calls);
}

/** 换身份重装门面：**两个方向**都要做，且画布闩与待落屏必须清零
 *
 *  身份现在可以运行期变（按键认领 / 收到识别帧），而门面是按身份装出来的：
 *  渲染目标与持久化钩子只给主卡——从主变从要撤、从从变主要装，**漏掉哪一边**
 *  在屏上都不报错（前者表现为"主卡的内容不落屏"，后者表现为"从卡画错地方"）。
 *
 *  画布与待落屏同样要清：身份一变，本卡那一块矩形就换了，留着旧内容 = 把别处的
 *  画面推上屏；`s_pending_flag` 留着更糟——会把一张刚被清空的画布立刻推下去。 */
static void case_identity_reapply(void)
{
    TEST_BEGIN("换身份重装门面：钩子两个方向都动，画布闩与待落屏清零");

    /* 板级网格钉在 1×2（见文件头）：格 0 = addr 0（主卡）、格 1 = addr 1（从卡）——
       **两种身份都在表里**，于是 `_apply_identity` 的"装"与"撤"两条路都能走到。
       而"s_grid 1×1 时从卡会落到门面停用"那条路由 case_self_addr_not_in_table 覆盖。 */
    canvas_reset(44, 12, 1, 2);
    s_target_last = nullptr;
    s_hook_last   = nullptr;

    /* ① 本卡 = 主卡（addr 0，格 0）→ 两种钩子都装上 */
    app_screen_set_addr(0);
    app_screen_reinit_identity();
    CHECK_MSG(s_target_last != nullptr, "主卡必须装上渲染目标（画布要靠它接渲染）");
    CHECK_MSG(s_hook_last != nullptr, "主卡必须装上持久化钩子");
    CHECK_MSG(app_screen_is_master(), "addr=0 应判为主卡");
    CHECK_MSG(app_screen_self_index() == 0, "本卡（addr 0）应落在格 0，得到 %u",
              (unsigned)app_screen_self_index());

    /* ② 写一次画布 → 闩与待落屏都置位（级联靠它们闸开轮） */
    _sink_fill(nullptr, 0, 0, 4, 4, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(app_screen_canvas_touched(), "画布被写过之后闩应置位");
    CHECK_MSG(s_pending_flag, "写画布应留下「待落屏」");

    /* ③ 变成从卡（addr 1，格 1，**仍在表里**）→ 两个钩子都要撤掉，画布与待落屏清零 */
    app_screen_set_addr(1);
    app_screen_reinit_identity();
    CHECK_MSG(s_target_last == nullptr, "从主变从要撤掉渲染目标（从卡不画本地画布）");
    CHECK_MSG(s_hook_last == nullptr, "从主变从也要撤掉持久化钩子");
    CHECK_MSG(!app_screen_is_master(), "addr=1 应判为从卡");
    CHECK_MSG(app_screen_self_index() == 1, "本卡（addr 1）应落在格 1，得到 %u",
              (unsigned)app_screen_self_index());
    CHECK_MSG(!app_screen_canvas_touched(), "换身份后画布闩要清零（新身份的画布是新的一份）");
    CHECK_MSG(!s_pending_flag, "待落屏也要清 —— 否则会立刻把一张刚清空的画布推给所有从卡");

    /* ④ 再换回主卡 → 钩子必须**装回去**（漏了这半边：从卡按键之后主卡再也不落屏） */
    app_screen_set_addr(0);
    app_screen_reinit_identity();
    CHECK_MSG(s_target_last != nullptr && s_hook_last != nullptr,
              "从从变主必须重新装上钩子（漏了这半边，按过键之后就再也不落屏）");
    CHECK_MSG(app_screen_is_master() && app_screen_self_index() == 0, "换回主卡后定位也要跟上");

    /* 本用例动了全局身份 —— **必须还原**：后面几条用例依赖 BOARD_CASC_ADDR 那个值 */
    app_screen_set_addr((uint8_t)BOARD_CASC_ADDR);
    canvas_reset(44, 12, 1, 2);
}

/** 输出颜色覆盖：工厂逐色老化要的（画布是 1bpp，颜色只能逐卡给一次）
 *
 *  现场症状：十种纯色填充全显示成绿色 —— 因为画布只记亮/灭，颜色来自切分表里
 *  本卡那一项。逐色老化只能靠"这一轮所有卡统一按某个颜色输出"来表达。
 *
 *  反向验证：把 `commit_self` 里的 `app_screen_output_color(s_color)` 换回 `s_color`，
 *  本用例第二条立刻红。 */
/** 渲染传下来的**颜色**要落到屏上（画布只记亮/灭，颜色另行记着）
 *
 *  现场症状：LDI 发红字显示成绿、RLS 的位图颜色同样被吃掉 —— 因为开画布之后
 *  "颜色由像素属于哪张卡决定"，渲染传下来的颜色整段丢了。
 *  RLS / VMS / AHMQ 走的是同一条路（都是 app_render），所以**一起修**。
 *
 *  反向验证：把 `app_screen_output_color` 里的内容色那一条去掉（退回只看卡片色），
 *  本用例第一条立刻红。 */
/** 持久化恢复也要带回颜色（记录里那个颜色字段不能丢在半路）
 *
 *  恢复这条路是**直写画布**、不经过渲染 sink，所以内容色要在重建画布时补记 ——
 *  否则上电恢复出来的内容会退回切分表给本卡的颜色（存在记录里的颜色白存了）。 */
static void case_restore_color(void)
{
    TEST_BEGIN("持久化恢复：内容颜色跟着回来（记录里存的就是它）");

    canvas_reset_mc(44, 12, 1, 2, 1); /* 本卡（addr=1）是格 0 */
    s_restore_ok    = true;
    s_restore_color = DEV_DISPLAY_COLOR_RED;

    CHECK_MSG(_persist_restore(), "恢复应成功（桩按记录里的颜色画实屏）");
    CHECK_MSG(app_screen_commit_self(), "落屏应成功");
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_RED,
              "恢复出来的内容该是记录里的红，得到 %u（本卡色 %u）", (unsigned)s_fb[0],
              (unsigned)BOARD_SCREEN_COLOR);

    /* 换一种颜色再恢复一次：不能记着上一次的 */
    s_restore_color = DEV_DISPLAY_COLOR_BLUE;
    CHECK_MSG(_persist_restore(), "第二次恢复应成功");
    app_screen_commit_self();
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_BLUE, "第二次恢复该是蓝，得到 %u", (unsigned)s_fb[0]);

    s_restore_ok = false;
}

static void case_content_color(void)
{
    TEST_BEGIN("内容颜色：单色内容用它的颜色，混色才退回本卡颜色");

    canvas_reset_mc(44, 12, 1, 2, 1); /* 本卡（addr=1）是格 0，矩形在画布原点 */
    static const uint8_t bm8[8] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};

    /* ① 清屏（黑）+ 红色内容 → 屏上应是**红**，不是切分表给本卡的绿 */
    _sink_fill(nullptr, 0, 0, 44, 24, DEV_DISPLAY_COLOR_BLACK); /* 整屏清屏（= 新一帧开始） */
    _sink_bitmap(&s_target, 0, 0, 8, 8, bm8, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(app_screen_commit_self(), "落屏应成功");
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_RED, "内容是红的，屏上就该是红的（得到 %u，本卡色 %u）",
              (unsigned)s_fb[0], (unsigned)BOARD_SCREEN_COLOR);

    /* ② 换成蓝色文字 → 跟着变蓝（颜色随内容走，不是锁死一个） */
    _sink_fill(nullptr, 0, 0, 44, 24, DEV_DISPLAY_COLOR_BLACK);
    _sink_bitmap(&s_target, 0, 0, 8, 8, bm8, DEV_DISPLAY_COLOR_BLUE);
    app_screen_commit_self();
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_BLUE, "换成蓝的之后就该是蓝的，得到 %u",
              (unsigned)s_fb[0]);

    /* ③ 一帧里混用两种非黑颜色 → **退回本卡颜色**（1bpp 表达不了多色，
       与其静默挑一个，不如退回这张卡的部署色 —— 现场看得见且可解释） */
    _sink_fill(nullptr, 0, 0, 44, 24, DEV_DISPLAY_COLOR_BLACK);
    _sink_fill(nullptr, 0, 0, 4, 4, DEV_DISPLAY_COLOR_RED);
    _sink_fill(nullptr, 8, 0, 4, 4, DEV_DISPLAY_COLOR_BLUE);
    app_screen_commit_self();
    CHECK_MSG(s_fb[0] == (uint8_t)BOARD_SCREEN_COLOR, "混色内容应退回本卡颜色 %u，得到 %u",
              (unsigned)BOARD_SCREEN_COLOR, (unsigned)s_fb[0]);

    /* ④ 清屏之后重新计数：上一帧的混色不该影响这一帧 */
    _sink_fill(nullptr, 0, 0, 44, 24, DEV_DISPLAY_COLOR_BLACK);
    _sink_fill(nullptr, 0, 0, 4, 4, DEV_DISPLAY_COLOR_YELLOW);
    app_screen_commit_self();
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_YELLOW, "新一帧只有一种颜色，应是黄的，得到 %u",
              (unsigned)s_fb[0]);
}

static void case_color_override(void)
{
    TEST_BEGIN("输出颜色覆盖：覆盖生效 / 取消后回到本卡颜色");

    canvas_reset_mc(44, 12, 1, 2, 1); /* 1×2、主卡在下：本卡（addr=1）是格 0，在画布原点 */
    _sink_fill(nullptr, 0, 0, 4, 4, DEV_DISPLAY_COLOR_WHITE);

    /* ① 没有覆盖：用**这一帧内容的颜色**（本用例画的是白色）。
       内容色 → 卡片色的优先关系见 case_content_color */
    app_screen_set_color_override(0xFF);
    CHECK_MSG(app_screen_commit_self(), "落屏应成功");
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_WHITE,
              "无覆盖时应是这一帧内容的颜色（白），得到 %u", (unsigned)s_fb[0]);

    /* ② 覆盖成红：同样的内容，实屏变红（整设备同色 —— 逐色老化的表达方式） */
    app_screen_set_color_override(DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(app_screen_commit_self(), "落屏应成功");
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_RED, "有覆盖时实屏应取覆盖色，得到 %u",
              (unsigned)s_fb[0]);

    /* ③ 取消覆盖：回到这一帧内容的颜色（老化轮播不该被测试用的颜色带着走） */
    app_screen_set_color_override(0xFF);
    CHECK_MSG(app_screen_commit_self(), "落屏应成功");
    CHECK_MSG(s_fb[0] == (uint8_t)DEV_DISPLAY_COLOR_WHITE, "取消覆盖后应回到内容色（白），得到 %u",
              (unsigned)s_fb[0]);
}

/** 主卡格是**运行期事实**（F3）：同一地址在不同主卡格下对应**不同的格**
 *
 *  这就是"谁被按谁主卡"能不能成立的分水岭 —— 只改地址不改格的话，被按的那张卡
 *  按老规矩去渲染**另一块屏**那一格，两块屏的上下半幅当场对调。
 *
 *  反向验证：把 `_layout_build()` 里的 `s_master_cell` 换回宏，第 ③ 段立刻红。 */
static void case_master_cell_runtime(void)
{
    TEST_BEGIN("主卡格可运行期改：同一地址换算出不同的格");

    /* ① 规则本身：格 ↔ 地址（主卡格编 0，其余按格序编 1..N） */
    CHECK_MSG(app_screen_addr_of_cell(0, 0) == 0 && app_screen_addr_of_cell(1, 0) == 1,
              "主卡格 0：格0→addr0、格1→addr1");
    CHECK_MSG(app_screen_addr_of_cell(0, 1) == 1 && app_screen_addr_of_cell(1, 1) == 0,
              "主卡格 1（主卡在下）：格0→addr1、格1→addr0");
    CHECK_MSG(app_screen_cell_of_addr(0, 1) == 1 && app_screen_cell_of_addr(1, 1) == 0,
              "反算：主卡格 1 下 addr0 在格1、addr1 在格0");
    for (uint8_t mc = 0; mc < 2; mc++)
        for (uint8_t a = 0; a < 2; a++)
            CHECK_MSG(app_screen_addr_of_cell(app_screen_cell_of_addr(a, mc), mc) == a,
                      "格↔地址来回一趟必须回到原点（mc=%u addr=%u）", (unsigned)mc, (unsigned)a);

    /* ② 本卡那一格跟着主卡格走。本用例本卡 addr=1（见文件头） */
    canvas_reset_mc(44, 12, 1, 2, 0); /* 主卡格 0（左上） */
    app_screen_apply_identity(1, 0);
    CHECK_MSG(app_screen_self_index() == 1, "主卡格 0 时本卡（addr1）在格 1，得到 %u",
              (unsigned)app_screen_self_index());

    app_screen_apply_identity(1, 1); /* 主卡格翻到下面那块 —— 与现场"按上面那块"等价 */
    CHECK_MSG(s_master_cell == 1, "主卡格应更新到 1，得到 %u", (unsigned)s_master_cell);
    CHECK_MSG(app_screen_self_index() == 0, "主卡格 1 时同一个 addr1 落在格 0，得到 %u",
              (unsigned)app_screen_self_index());
    CHECK_MSG(s_target_last == nullptr, "本卡是 addr1（从卡）→ 不该装渲染目标");
    CHECK_MSG(!app_screen_canvas_touched(),
              "换了格就要清画布与闩（换身份后那份内容不算完整的一幅）");

    /* ③ **一个都没变时不许动任何东西**：按一下键不该把屏上内容清掉 */
    app_screen_apply_identity(0, 1); /* 本卡成为主卡（格 1） */
    _sink_fill(nullptr, 0, 0, 4, 4, DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(app_screen_canvas_touched(), "画布被写过之后闩应置位");
    app_screen_apply_identity(0, 1); /* 一模一样 */
    CHECK_MSG(app_screen_canvas_touched() && app_screen_is_master(),
              "身份一个都没变时不该重装门面（重装会清掉刚画好的内容）");

    app_screen_set_addr((uint8_t)BOARD_CASC_ADDR);
    app_screen_apply_identity((uint8_t)BOARD_CASC_ADDR, 0);
}

static void case_commit_self_matches_canvas(void)
{
    TEST_BEGIN("本卡落屏：实屏内容 == 本卡矩形在画布上的内容");

    canvas_reset(44, 12, 1, 2); /* 本卡 = 1 号卡，矩形 44x12@(0,12)：y 偏移非 0 */
    paint_pattern();

    CHECK_MSG(app_screen_commit_self(), "本卡落屏应成功");

    const app_screen_card_t *c = &app_screen_layout()->cards[app_screen_self_index()];
    uint8_t              bm[BM_MAX];
    CHECK_MSG(app_screen_extract(app_screen_self_index(), bm, sizeof(bm)), "抽带应成功");

    /* 逐像素比对：实屏（1B/px，本卡尺寸）× 画布（1bpp）里本卡那块矩形 */
    uint16_t diff  = 0;
    uint16_t stride = (uint16_t)((c->w + 7) / 8);
    for (uint16_t y = 0; y < s_h; y++)
        for (uint16_t x = 0; x < s_w; x++) {
            bool on_screen = (s_fb[(uint32_t)y * s_w + x] != DEV_DISPLAY_COLOR_BLACK);
            bool on_canvas = (bm[(uint32_t)y * stride + (x >> 3)] & (uint8_t)(0x80U >> (x & 7U))) != 0;
            if (on_screen != on_canvas) diff++;
        }
    CHECK_MSG(diff == 0, "实屏与画布有 %u 个像素不一致", (unsigned)diff);

    /* 落屏用的颜色必须是**本卡**的（切分表逐卡给），不是全局默认 */
    CHECK_MSG(s_fb[0] == DEV_DISPLAY_COLOR_BLACK || s_fb[0] == c->color, "落屏颜色不是本卡那一色");
}

/** 主卡在**下面**那一块 —— 现场的实际拼法。地址不按网格下标编。 */
static void case_master_below(void)
{
    TEST_BEGIN("主卡在下方：地址按「主卡那格编 0」分配，抽带仍按矩形");

    /* 1×2 网格：格 0 在上、格 1 在下；主卡是**下面那块** → 格 1 编 addr 0 */
    canvas_reset_mc(48, 16, 1, 2, 1);
    const app_screen_layout_t *L = app_screen_layout();

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

    const app_screen_card_t *bottom = app_screen_card(app_screen_index_of_addr(0)); /* 正确用法 */
    const app_screen_card_t *oops   = app_screen_card(0);                          /* 地址当下标 */

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

/* ---- 告警监听：只记调用，不做任何事 ---- */
static int     s_alarm_calls;
static uint8_t s_alarm_addr;
static uint8_t s_alarm_st;

static void on_alarm(uint8_t addr, uint8_t st)
{
    s_alarm_calls++;
    s_alarm_addr = addr;
    s_alarm_st   = st;
}

/** 状态快照与可选告警：**不注册就完全静默**，注册后只在跳变时触发 */
static void case_status_and_alarm(void)
{
    TEST_BEGIN("状态快照与告警：不注册完全静默；注册后只在状态真的变了时触发");

    canvas_reset_mc(48, 16, 1, 2, 1); /* 下标 0 = addr 1（从卡），下标 1 = addr 0（本卡） */

    app_screen_status_t st;
    app_screen_status(&st);
    CHECK_MSG(st.online_mask == 0, "刚建表谁都不在线，online_mask 应为 0，得到 %02X",
              (unsigned)st.online_mask);
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_MISSING,
              "建表初值应是 MISSING（= 从未应答过，多半是配置错），得到 %u",
              (unsigned)app_screen_card_state(0));

    /* **没注册监听：改状态不产生任何回调** —— 这是"上报是可选的"那条决策的落点 */
    app_screen_register_alarm(nullptr);
    s_alarm_calls = 0;
    app_screen_card_set_state(0, APP_SCREEN_CARD_STATE_ONLINE);
    CHECK_MSG(s_alarm_calls == 0, "没注册监听时不该触发任何回调");

    app_screen_status(&st);
    CHECK_MSG(st.online_mask == 0x01, "addr 1 在线 → online_mask 的位 0 置起，得到 %02X",
              (unsigned)st.online_mask);
    CHECK_MSG(st.evict_cnt == 0, "上线不算剔除");

    app_screen_register_alarm(on_alarm);

    s_alarm_calls = 0;
    app_screen_card_set_state(0, APP_SCREEN_CARD_STATE_ONLINE); /* 状态没变 */
    CHECK_MSG(s_alarm_calls == 0, "状态没变不该触发告警 —— 否则上位机会被同一件事反复打扰");

    app_screen_card_set_state(0, APP_SCREEN_CARD_STATE_OFFLINE);
    CHECK_MSG(s_alarm_calls == 1 && s_alarm_addr == 1 && s_alarm_st == APP_SCREEN_CARD_STATE_OFFLINE,
              "剔除应触发一次告警（addr=1, OFFLINE），得到 %d 次 addr=%u st=%u", s_alarm_calls,
              (unsigned)s_alarm_addr, (unsigned)s_alarm_st);

    app_screen_status(&st);
    CHECK_MSG(st.online_mask == 0, "剔除后应不在线");
    CHECK_MSG(st.evict_cnt == 1, "剔除计数应为 1，得到 %u", (unsigned)st.evict_cnt);
    CHECK_MSG(st.last_alarm == 1, "last_alarm 应是刚跳变的卡地址，得到 %u",
              (unsigned)st.last_alarm);

    app_screen_note_retrans();
    app_screen_note_retrans();
    app_screen_note_round(0x1234);
    app_screen_status(&st);
    CHECK_MSG(st.retrans_cnt == 2, "重传计数应为 2，得到 %u", (unsigned)st.retrans_cnt);
    CHECK_MSG(st.seq_lo == 0x34, "序号只取低 8 位，应为 34，得到 %02X", (unsigned)st.seq_lo);

    /* 越界一律安全返回，不越界读 */
    CHECK_MSG(app_screen_card_state(9) == APP_SCREEN_CARD_STATE_MISSING, "越界下标应返回 MISSING");
    app_screen_card_set_state(9, APP_SCREEN_CARD_STATE_OFFLINE); /* 不该崩、不该改到别的卡 */
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_OFFLINE, "越界写入不该动到别的卡");
    app_screen_status(nullptr); /* 空指针不该崩 */

    app_screen_register_alarm(nullptr); /* 别影响后面的用例 */
}

/** 地址不在切分表里：停用门面，**不静默降级**成"单卡占满" */
static void case_self_addr_not_in_table(void)
{
    TEST_BEGIN("本卡地址不在切分表里 → 停用，不降级");

    display_reset(48, 16);
    s_display_dev = &s_dev;

    /* 板级网格是 1×2（addr 0 与 1 两张卡），而本卡被摆成 addr=2 —— 表里没有它 */
    app_screen_set_addr(2);
    _screen_init();

    CHECK_MSG(s_display_dev == nullptr, "地址不在表里时整屏门面应停用，而不是自己占满整屏");

    /* 本用例动了全局身份：还原（文件头钉的就是 BOARD_CASC_ADDR=1） */
    app_screen_set_addr((uint8_t)BOARD_CASC_ADDR);
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m切分表与抽带（本卡 addr=%u）\033[0m\n", (unsigned)BOARD_CASC_ADDR);

    case_grid_shape();
    case_extract_grids();
    case_extract_unaligned();
    case_extract_bounds();
    case_persist_after_commit();
    case_identity_reapply();
    case_color_override();
    case_content_color();
    case_restore_color();
    case_master_cell_runtime();
    case_commit_self_matches_canvas();
    case_master_below();
    case_addr_index_mapping();
    case_status_and_alarm();
    case_self_addr_not_in_table();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
