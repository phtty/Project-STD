/**
 * @file    test_screen_canvas.c
 * @brief   整屏门面：1bpp 画布与"直写实屏"必须产出同一幅画面
 *
 * **为什么需要这个测试**：画布把渲染路径从"逐字符直写 pixel_map"改成"写 1bpp 位掩码、
 * 静默期后整屏落一次"。这条路径要是错了，表现是**某个字、某条边、某个角**不对 ——
 * 上机靠人眼比对很不靠谱，而且级联一上来之后"主卡上看到的"与"发出去的"必须是同一份，
 * 画布错了会同时污染主从两侧，很难归因。这里用一份**独立的参考实现**（直写实屏 +
 * 按位打包）把两者钉成逐像素相等。
 *
 * 参考实现是刻意另写的，不是把被测代码的算法抄一遍：它走的是 `dev_display_*` 直写
 * `pixel_map`，然后按 `(宽+7)/8`、MSB-first 打包 —— 与级联要用的线格式同源但路径无关。
 *
 * 被测代码是生产源码本体：直接 include `app_screen.c`（它的 sink 是 static，
 * 从外部够不着；这也顺带让 `#if BOARD_SCREEN_CANVAS` 里的代码真正进到构建里）。
 */

/* 必须在 include app_screen.c 之前定义：board.h 里是 #ifndef 保护的，
   否则整段画布代码会被编译掉，本用例就变成空跑。 */
#define BOARD_SCREEN_CANVAS 1

/* **钉住切分参数，不跟着 board.h 的现场配置变**：本用例要测的是"单卡 / 主卡在原点"
   这批基准行为，而 board.h 是**部署**配置（现场可能是 1×2、主卡在下）。
   跟着它变的话，同一份测试在别人的板子上会测出不同结论 —— 而且不会报错，
   只会静默地少测几条。 */
#define BOARD_CASC_COLS        1
#define BOARD_CASC_ROWS        1
#define BOARD_CASC_MASTER_CELL 0
#define BOARD_CASC_ADDR        0

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
/* _screen_init 会建任务；本用例手动驱动提交，不需要真的起线程 */
osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn; (void)arg; (void)attr;
    return nullptr;
}

void app_render_set_target(const app_render_target_t *t) { (void)t; }
void app_render_set_persist_hook(const app_render_persist_hook_fn_t *h) { (void)h; }
void app_render_save(void) {}
bool app_render_restore(void) { return false; }
/* "这一帧要落盘"的请求位：本套件桩成"从没有过请求"，落屏路径照跑 */
bool app_render_take_persist_req(void) { return false; }
bool app_render_peek_persist_req(void) { return false; }

/* ---- dev_display 原语：本用例的参考实现 ----
 *
 * 按真 dev_display.c 的语义写（含那两个坑：fill 只裁右下、draw_bitmap 越界整体放弃），
 * 因为画布要**产出与它相同的结果**，语义抄错就失去比对的意义。 */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color)
{
    if (x < dev->screen_rows && y < dev->screen_cols) {
        dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
        dev->dirty                               = true;
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


/* ---- 被测：生产源码本体 ---- */
#include "../Application/Src/app_screen.c"

/* ================================================================
 *  测试夹具
 * ================================================================ */

#define W (48U)
#define H (16U)

static dev_display_t s_dev;
static uint8_t       s_fb[W * H];

static void display_reset(void)
{
    memset(s_fb, DEV_DISPLAY_COLOR_BLACK, sizeof(s_fb));
    s_dev.screen_rows = W;
    s_dev.screen_cols = H;
    s_dev.pixel_map   = s_fb;
    s_dev.light_level = 7;
    s_dev.dirty       = false;
}

dev_display_t *dev_display_get(void) { return &s_dev; }

/** @brief 走真实的初始化路径把画布几何对齐到本夹具（先清屏、再 _screen_init、再清待提交）
 *
 *  不用测试专属的"设置几何"后门：`_screen_init` 就是生产里填几何与清画布的**唯一**路径，
 *  绕开它测出来的东西不能代表真机。 */
static void canvas_reset(void)
{
    display_reset();
    _screen_init();
    s_pending_flag = false; /* 手动比对，不要后台任务来插一脚 */
}

/** @brief 独立参考：逐像素把 pixel_map 打包成 1bpp（(宽+7)/8 行字节、MSB-first、黑=0） */
static void pack_reference(const uint8_t *fb, uint16_t rows, uint16_t cols, uint8_t *out)
{
    uint16_t row_bytes = (rows + 7) / 8;
    memset(out, 0, row_bytes * cols);
    for (uint16_t y = 0; y < cols; y++)
        for (uint16_t x = 0; x < rows; x++)
            if (fb[y * rows + x] != DEV_DISPLAY_COLOR_BLACK)
                out[y * row_bytes + x / 8] |= (uint8_t)(0x80U >> (x % 8));
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

/** @brief 逐位比对两张位图，给出第一个不同的位置 */
static void check_bitmaps_equal(const uint8_t *a, const uint8_t *b, uint16_t len, const char *what)
{
    for (uint16_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            CHECK_MSG(0, "%s 第 %u 字节不同：画布 %02X / 参考 %02X", what, (unsigned)i, a[i], b[i]);
            return;
        }
    }
    CHECK_MSG(1, "%s", what);
}

/* ================================================================
 *  用例
 * ================================================================ */

/** 画布路径（走 sink）与直写实屏（走 dev_display_*）必须产出同一幅画面 */
static void case_canvas_matches_direct(void)
{
    TEST_BEGIN("经画布渲染 == 直写实屏渲染（单色内容逐像素相等）");

    uint8_t ref[W * H], got[W * H];

    /* ---- 参考：直写实屏，然后打包 ---- */
    display_reset();
    dev_display_fill(&s_dev, 0, 0, W, H, DEV_DISPLAY_COLOR_BLACK);
    for (uint16_t y = 0; y < H; y += 3)
        dev_display_fill(&s_dev, 4, y, 20, 1, DEV_DISPLAY_COLOR_RED); /* 横条 */
    for (uint16_t x = 0; x < W; x += 8)
        dev_display_fill(&s_dev, x, 0, 1, H, DEV_DISPLAY_COLOR_RED); /* 每 8 像素一竖条 */
    pack_reference(s_fb, W, H, ref);

    /* ---- 被测：同一组操作走画布 ---- */
    canvas_reset();
    _sink_fill(nullptr, 0, 0, W, H, DEV_DISPLAY_COLOR_BLACK);
    for (uint16_t y = 0; y < H; y += 3)
        _sink_fill(nullptr, 4, y, 20, 1, DEV_DISPLAY_COLOR_RED);
    for (uint16_t x = 0; x < W; x += 8)
        _sink_fill(nullptr, x, 0, 1, H, DEV_DISPLAY_COLOR_RED);
    memcpy(got, s_canvas_buf, sizeof(got));

    check_bitmaps_equal(got, ref, sizeof(got), "横条+竖条");
}

/** 位图叠加路径：bit=1 才写、bit=0 不动，与 dev_display_draw_bitmap 同语义 */
static void case_bitmap_overlay_semantics(void)
{
    TEST_BEGIN("bitmap 叠加语义与 draw_bitmap 一致（bit=0 不清除）");

    uint8_t bm[6 * 2];
    memset(bm, 0, sizeof(bm));
    bm[0] = 0xF0; /* 第一行左 4 位 */
    bm[1] = 0x0F; /* 第一行接着 4 位 */

    uint8_t ref[W * H], got[W * H];

    /* 参考：先铺底再叠一层 */
    display_reset();
    dev_display_fill(&s_dev, 0, 0, 8, 2, DEV_DISPLAY_COLOR_GREEN);
    dev_display_draw_bitmap(&s_dev, 0, 0, 8, 1, bm, DEV_DISPLAY_COLOR_BLACK); /* 黑色位图 = 擦 */
    pack_reference(s_fb, W, H, ref);

    /* 被测：同样两步走画布 */
    canvas_reset();
    _sink_fill(nullptr, 0, 0, 8, 2, DEV_DISPLAY_COLOR_GREEN);
    _sink_bitmap(nullptr, 0, 0, 8, 1, bm, DEV_DISPLAY_COLOR_BLACK);
    memcpy(got, s_canvas_buf, sizeof(got));

    check_bitmaps_equal(got, ref, sizeof(got), "黑位图擦除");
}

/** 越界：画布必须自己裁干净，且不得写坏自己的缓冲 */
static void case_clipping(void)
{
    TEST_BEGIN("越界写入被裁掉，不越界也不写坏画布");

    canvas_reset();

    /* 这些在图真 dev_display 上分别会：下溢冲出缓冲 / 整体放弃 */
    _sink_fill(nullptr, W + 10, 0, 4, 4, DEV_DISPLAY_COLOR_RED);   /* x 越界 */
    _sink_fill(nullptr, 0, H + 10, 4, 4, DEV_DISPLAY_COLOR_RED);   /* y 越界 */
    _sink_fill(nullptr, W - 2, H - 2, 100, 100, DEV_DISPLAY_COLOR_RED); /* 右下溢出 */
    _sink_bitmap(nullptr, W - 4, 0, 8, 1, s_fb, DEV_DISPLAY_COLOR_RED); /* 源可能越界，画布要裁 */
    _sink_set_pixel(nullptr, W + 1, 0, DEV_DISPLAY_COLOR_RED);
    _sink_set_pixel(nullptr, 0, H + 1, DEV_DISPLAY_COLOR_RED);

    uint16_t stride = (W + 7) / 8;

    /* 画布尾部（几何之外的定长池余量）必须仍是 0 —— 越界写会踩到这里 */
    bool tail_clean = true;
    for (uint32_t i = (uint32_t)stride * H; i < sizeof(s_canvas_buf); i++)
        if (s_canvas_buf[i]) { tail_clean = false; break; }
    CHECK_MSG(tail_clean, "越界写踩到了画布尾部（几何之外的池余量不再是 0）");
    CHECK_MSG((s_canvas_buf[(H - 1) * stride + (W - 1) / 8] & (0x80U >> ((W - 1) % 8))) != 0,
              "右下角溢出调用没有裁到位");
    /* 越界的那几次不得在画布上留下任何东西 */
    CHECK_MSG(s_canvas_buf[0] == 0, "越界 fill 写进了画布左上角");
}

/** APP_RENDER_TYPE_FILL 的 w=h=0 全屏语义走的是目标几何，不是实屏几何 */
static void case_fullscreen_fill(void)
{
    TEST_BEGIN("全屏填充覆盖整块画布");

    canvas_reset();
    _sink_fill(nullptr, 0, 0, W, H, DEV_DISPLAY_COLOR_WHITE);

    uint16_t stride = (W + 7) / 8;
    uint16_t ones   = 0;
    for (uint16_t i = 0; i < stride * H; i++)
        for (uint8_t b = 0; b < 8; b++)
            if (s_canvas_buf[i] & (0x80U >> b)) ones++;

    /* 48 宽 × 16 高，末字节补位不算 —— 全屏白应该正好 W*H 个 1 */
    CHECK_MSG(ones == W * H, "置位数 %u != %u（末字节补位或裁剪有问题）", (unsigned)ones,
              (unsigned)(W * H));
}

/** 落屏：位图长度不符必须拒绝，而不是将就出一幅错位画面 */
static void case_commit_length_guard(void)
{
    TEST_BEGIN("落屏拒绝长度不符的位图");

    canvas_reset();
    uint8_t good[W * H / 8];
    memset(good, 0xFF, sizeof(good));

    /* 先把屏画花，好分辨"有没有被动过" */
    dev_display_fill(&s_dev, 0, 0, W, H, DEV_DISPLAY_COLOR_RED);

    app_screen_commit_bitmap(good, (uint16_t)(sizeof(good) - 1), DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(s_fb[0] == DEV_DISPLAY_COLOR_RED, "长度不符却被落屏了");

    app_screen_commit_bitmap(good, (uint16_t)sizeof(good), DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(s_fb[0] == DEV_DISPLAY_COLOR_GREEN, "长度正确却没落屏");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m整屏画布（几何 %ux%u）\033[0m\n", (unsigned)W, (unsigned)H);

    case_canvas_matches_direct();
    case_bitmap_overlay_semantics();
    case_clipping();
    case_fullscreen_fill();
    case_commit_length_guard();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
