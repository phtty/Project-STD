/**
 * @file    app_screen.c
 * @brief   整屏门面实现 —— 1bpp 逻辑画布 + 渲染目标 + 落屏 + 亮度
 *
 * 见 app_screen.h 的设计说明。本文件在 P1（画布与渲染目标）阶段的形态：
 * **不开级联**，逻辑几何 = 本屏几何，画布画完直接落到本地屏。
 * 级联接入后几何来自切分表、提交改为"抽取本卡矩形 + 逐卡下发"。
 *
 * 默认关闭（board.h 的 BOARD_SCREEN_CANVAS）：单独一块卡上，画布是纯开销 ——
 * 它多占一块 1bpp 缓冲、多一次拷贝，还要把卡内多色塌缩成单色，却没有换来任何功能。
 * 等级联落地时再打开。中间靠 `app_render_set_target(NULL)` 即可完全回退。
 */

#include "app_screen.h"

#include <stdio.h>
#include <string.h>
#include "board.h" /* BOARD_SCREEN_CANVAS / _CANVAS_MAX / _COLOR */
#include "app_render.h"
#include "dev_display.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_task.h"

/* ---- 画布 ----
 * 1bpp 字节池。静态定长、按"本板参与的最大级联规模"留，运行期几何从中切 ——
 * dev_display_t 没有编译期几何宏，尺寸只能运行期读（与本工程既有做法一致）。
 *
 * 放 SRAM 不放 CCMRAM：画布只在渲染与抽取时被碰（每轮几 KB），是冷路径，
 * 不值得跟协议 RB/队列（工程惯例放 CCMRAM）与显示缓冲抢那 64KB。
 * 且 CCMRAM 在 5006048 上只剩约 12KB，1B/px 的整屏画布根本放不下 ——
 * "整屏 1bpp"这个决策的真实价值就在这里。 */
/* ---- 以下全部依赖画布：开关关闭时整段不进构建（省下画布池的 SRAM）---- */
#if BOARD_SCREEN_CANVAS
static uint8_t s_canvas[BOARD_SCREEN_CANVAS_MAX];
#endif /* BOARD_SCREEN_CANVAS */

static dev_display_t *s_display;
static uint16_t       s_rows;   /* 逻辑宽（P1 = 本屏宽；级联后来自切分表） */
static uint16_t       s_cols;   /* 逻辑高 */
static uint16_t       s_stride; /* = (s_rows + 7) / 8 */
static uint16_t       s_bm_len; /* = s_stride * s_cols */

[[maybe_unused]] static uint8_t s_color = BOARD_SCREEN_COLOR; /**< 本卡颜色（级联后由切分表逐卡给） */

static volatile uint32_t s_gen;             /* 内容代数：每次写入自增 */
static volatile uint32_t s_last_write_tick; /* 最后一次写入的时刻，静默期据此算 */
static volatile bool     s_pending;         /* 有内容尚未落屏 */

#define SCREEN_POLL_MS   (10U) /**< 检查静默期的周期 */
#define SCREEN_SETTLE_MS (50U) /**< 静默多久算"这一屏画完了" */

uint32_t app_screen_generation(void)
{
    return s_gen;
}

/* ================================================================
 *  落屏：主卡本地提交与从卡落屏**共用的唯一路径**
 * ================================================================ */

void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color)
{
    dev_display_t *d = s_display;
    if (!d || !bm) return;

    /* 长度必须正好是本屏的位图长度。不符说明这块内容不是给这块屏的
       （换模组、或主从卡几何不一致），拒绝而不是将就 —— 将就的后果是错位画面。 */
    uint16_t need = (uint16_t)(((d->screen_rows + 7U) / 8U) * d->screen_cols);
    if (len != need) {
        printf("[screen] 位图长度 %u != 本屏 %u，已拒绝落屏\n", (unsigned)len, (unsigned)need);
        return;
    }

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    /* fill 与 draw 之间把 dirty 压住：否则 scan_task 可能正好在两步之间跑 prepare，
       屏上闪一帧全黑。draw_bitmap 结束时会把 dirty 置回，下一次 prepare 一次性换帧。 */
    d->dirty = false;
    dev_display_draw_bitmap(d, 0, 0, d->screen_rows, d->screen_cols, bm, (display_color_t)color);
}

/** @brief 把画布落到本地屏。
 *
 *  P1：逻辑几何 == 本屏几何，画布可以整块交出去。
 *  级联接入后这里要改成"按切分表抽出本卡那个矩形"，因为画布会大于本屏。 */
/* ---- 以下全部依赖画布：开关关闭时整段不进构建（省下画布池的 SRAM）---- */
#if BOARD_SCREEN_CANVAS
static void _commit_local(void)
{
    app_screen_commit_bitmap(s_canvas, s_bm_len, s_color);
}

/* ================================================================
 *  渲染目标实现
 *
 *  **边界裁剪必须自己做**：不顺开 dev_display 那两个坑 ——
 *  `dev_display_fill` 在 x > screen_rows 时 w 会 uint16 下溢、冲出缓冲；
 *  `dev_display_draw_bitmap` 越界则整体放弃且不报错。
 *  在画布层裁干净，两个坑就都用不上。
 * ================================================================ */

static void _mark_dirty(void)
{
    s_gen++;
    s_last_write_tick = osKernelGetTickCount();
    s_pending         = true;
}

/** @brief 把矩形裁到画布内；全裁掉返回 false */
static bool _clip(uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    if (*x >= s_rows || *y >= s_cols) return false;
    if ((uint32_t)*x + *w > s_rows) *w = (uint16_t)(s_rows - *x);
    if ((uint32_t)*y + *h > s_cols) *h = (uint16_t)(s_cols - *y);
    return *w && *h;
}

/** @brief 置/清一个像素位 */
static inline void _set_bit(uint16_t x, uint16_t y, bool on)
{
    uint8_t *p   = &s_canvas[(uint32_t)y * s_stride + (x >> 3)];
    uint8_t  msk = (uint8_t)(0x80U >> (x & 7U));
    if (on)
        *p |= msk;
    else
        *p &= (uint8_t)~msk;
}

static void _sink_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t c)
{
    (void)ctx;
    if (!_clip(&x, &y, &w, &h)) return;

    /* 画布只记亮/灭，**忽略具体是哪个非黑颜色** —— 最终颜色由像素属于哪张卡决定 */
    bool on = (c != COLOR_BLACK);

    for (uint16_t r = 0; r < h; r++) {
        uint8_t *row = &s_canvas[(uint32_t)(y + r) * s_stride];
        for (uint16_t k = 0; k < w; k++) {
            uint16_t xx = (uint16_t)(x + k);
            uint8_t  m  = (uint8_t)(0x80U >> (xx & 7U));
            if (on)
                row[xx >> 3] |= m;
            else
                row[xx >> 3] &= (uint8_t)~m;
        }
    }
    _mark_dirty();
}

static void _sink_bitmap(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint8_t *bm, display_color_t c)
{
    (void)ctx;
    if (!bm || !_clip(&x, &y, &w, &h)) return;

    /* 与 dev_display_draw_bitmap 同语义：bit=1 才写（写 on 或 off），bit=0 不动。
       位序同为 MSB-first、(宽+7)/8 行字节。 */
    bool         on        = (c != COLOR_BLACK);
    uint16_t     src_stride = (uint16_t)((w + 7U) / 8U);

    for (uint16_t r = 0; r < h; r++) {
        for (uint16_t k = 0; k < w; k++) {
            if (bm[(uint32_t)r * src_stride + (k >> 3)] & (uint8_t)(0x80U >> (k & 7U)))
                _set_bit((uint16_t)(x + k), (uint16_t)(y + r), on);
        }
    }
    _mark_dirty();
}

static void _sink_set_pixel(void *ctx, uint16_t x, uint16_t y, display_color_t c)
{
    (void)ctx;
    if (x >= s_rows || y >= s_cols) return;
    _set_bit(x, y, c != COLOR_BLACK);
    _mark_dirty();
}

/* ---- 渲染目标与持久化钩子（只在 BOARD_SCREEN_CANVAS 打开时注册）---- */

/* **不能加 const**：几何在 _screen_init 里填，而 const 对象住 .rodata ——
   在 STM32 上那就是 Flash，写进去会被静默丢弃（不报错！），于是 rows/cols 恒为 0，
   画布裁剪把所有内容裁光、屏上什么都不显示。host 单测用 ASan 抓到的就是这个写。 */
static render_target_t s_target = {
    .fill      = _sink_fill,
    .bitmap    = _sink_bitmap,
    .set_pixel = _sink_set_pixel,
    .ctx       = nullptr,
    .rows      = 0, /* _screen_init 里填 */
    .cols      = 0,
};

static void _persist_save(void);
static bool _persist_restore(void);
static const render_persist_hook_t s_persist_hook = {.save = _persist_save, .restore = _persist_restore};

/* ================================================================
 *  显存持久化：画布版本的存/取
 *
 *  接管之后存的**不再是实屏**，而是画布 —— 级联下这才是"整屏的内容"。
 *  格式沿用 render_persist_t（1bpp、MSB-first），但多带一个颜色字段已经够用
 *  （本卡颜色由切分表给，不随内容变）。
 *
 *  P1 阶段画布 == 本屏，所以存画布与存实屏等价；改动的意义在于接上钩子这条缝。
 * ================================================================ */

static void _persist_save(void)
{
    /* P1 画布 = 本屏，直接复用 app_render 原有的落盘路径：
       那里读的是 dev_display 的 pixel_map，而画布刚刚才提交给它，两者一致。
       级联接入后改为直接存画布（那时实屏只是整屏的一条带，不够）。
       此处显式写明这个前提，免得日后忘了改。 */
    app_render_set_persist_hook(nullptr);
    app_render_save();
    app_render_set_persist_hook(&s_persist_hook);
}

static bool _persist_restore(void)
{
    bool ok;
    app_render_set_persist_hook(nullptr);
    ok = app_render_restore();
    app_render_set_persist_hook(&s_persist_hook);

    /* 恢复了实屏，画布要跟着同步 —— 否则下一轮静默提交会拿一张空画布把屏刷黑。
       P1 下画布=实屏，由实屏重新打包即可。 */
    if (ok && s_display) {
        memset(s_canvas, 0, s_bm_len);
        for (uint16_t y = 0; y < s_cols; y++)
            for (uint16_t x = 0; x < s_rows; x++)
                if (s_display->pixel_map[(uint32_t)y * s_rows + x] != COLOR_BLACK)
                    s_canvas[(uint32_t)y * s_stride + (x >> 3)] |= (uint8_t)(0x80U >> (x & 7U));
        s_pending = false; /* 刚恢复的内容已经落过屏，不必再提交一遍 */
    }
    return ok;
}
#endif /* BOARD_SCREEN_CANVAS */

/* ================================================================
 *  亮度
 * ================================================================ */

static volatile bool    s_bright_pending;
static volatile uint8_t s_bright_level;

void app_screen_set_brightness(uint8_t level)
{
    if (level > 7) level = 7;
    if (s_display) dev_display_set_brightness(s_display, level);

    s_bright_level   = level;
    s_bright_pending = true; /* 由级联协议取走并广播给从卡 */
}

uint8_t app_screen_get_brightness(void)
{
    return s_display ? s_display->light_level : 0;
}

bool app_screen_brightness_take_pending(uint8_t *level)
{
    if (!s_bright_pending) return false;
    s_bright_pending = false;
    if (level) *level = s_bright_level;
    return true;
}

uint8_t app_screen_self_addr(void)
{
    return (uint8_t)BOARD_CASCADE_ADDR;
}

bool app_screen_is_master(void)
{
    return app_screen_self_addr() == 0U; /* 地址 0 = 主卡 */
}

void app_screen_flush(void)
{
    /* 把"最后一次写入"往前推过静默窗，下一次轮询就提交 */
    s_last_write_tick = osKernelGetTickCount() - SCREEN_SETTLE_MS;
    s_pending         = true;
}

/* ================================================================
 *  静默期自动提交
 *
 *  为什么不"每次 app_render 返回就提交"：
 *   · `vms_display_ctrl` 是"先 fill 再 bitmap"两次调用，逐次提交会推两遍整屏
 *   · 文字是**逐字** fill+draw_bitmap，中间态会被推出去（屏上会闪）
 *   · 调用点有六处，靠人记得 flush 迟早会漏
 *  静默 50ms 之后才提交，上述三类问题一次解决，且现有调用点一行都不用改。
 * ================================================================ */

/* ---- 以下全部依赖画布：开关关闭时整段不进构建（省下画布池的 SRAM）---- */
#if BOARD_SCREEN_CANVAS
static void _screen_task(void *arg)
{
    (void)arg;
    for (;;) {
        osDelay(SCREEN_POLL_MS);
        if (!s_pending) continue;
        if ((osKernelGetTickCount() - s_last_write_tick) < SCREEN_SETTLE_MS) continue;
        s_pending = false;
        _commit_local();
    }
}
#endif /* BOARD_SCREEN_CANVAS */

/* ================================================================
 *  初始化
 * ================================================================ */

static void _screen_init(void)
{
    dev_display_t *d = dev_display_get();
    if (!d) {
        printf("[screen] 显示未就绪，整屏门面停用\n");
        return;
    }
    s_display = d;

    /* P1：逻辑几何 = 本屏几何。级联接入后这里改成切分表给出的整屏几何。 */
    s_rows   = d->screen_rows;
    s_cols   = d->screen_cols;
    s_stride = (uint16_t)((s_rows + 7U) / 8U);
    s_bm_len = (uint16_t)(s_stride * s_cols);

#if BOARD_SCREEN_CANVAS
    if (s_bm_len > sizeof(s_canvas)) {
        /* 拦下而不是截断：画布小了的表现是"右边/下边一块永远不更新"，很难查 */
        printf("[screen] 画布需要 %u 字节 > BOARD_SCREEN_CANVAS_MAX %u，整屏门面停用\n",
               (unsigned)s_bm_len, (unsigned)sizeof(s_canvas));
        s_display = nullptr;
        return;
    }
    memset(s_canvas, 0, s_bm_len);

    s_target.rows = s_rows;
    s_target.cols = s_cols;

    app_render_set_persist_hook(&s_persist_hook);
    app_render_set_target(&s_target);

    const osThreadAttr_t attr = {
        .name       = "screen",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    pl_task_new(_screen_task, nullptr, &attr);
    printf("[screen] 逻辑画布 %ux%u（%u 字节，1bpp），颜色 %u\n", (unsigned)s_rows,
           (unsigned)s_cols, (unsigned)s_bm_len, (unsigned)s_color);
#else
    printf("[screen] 整屏门面未启用（BOARD_SCREEN_CANVAS=0），渲染直写实屏\n");
#endif
}
sw_dev_initcall(_screen_init);
