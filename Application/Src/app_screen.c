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
static uint16_t       s_rows;   /* **整屏**逻辑宽（单卡时 == 本屏宽） */
static uint16_t       s_cols;   /* 整屏逻辑高 */
static uint16_t       s_stride; /* = (s_rows + 7) / 8 */
static uint16_t       s_bm_len; /* = s_stride * s_cols */
static uint8_t        s_self;   /* 本卡在切分表里的下标 */

static uint8_t s_color = BOARD_SCREEN_COLOR; /**< 本卡颜色（来自切分表本卡那一项） */

/* 抽带缓冲：本卡矩形抽出来放这儿，再交给 commit_bitmap 落屏。
   主卡本地提交与"单卡即整屏"共用它 —— 长度由 BOARD_CASCADE_BAND_MAX 兜底，
   _screen_init 按**运行期几何**校验一次（超了会明确打出来并停用门面）。 */
#if BOARD_SCREEN_CANVAS
static uint8_t s_band[BOARD_CASCADE_BAND_MAX];
#endif

/* ================================================================
 *  切分表 —— 本期由 board.h 的网格参数合成
 *
 *  **整屏 = COLS×ROWS 张等尺寸卡**，每张卡占一整块"本卡屏几何"大小的矩形，
 *  地址 = 网格下标（行优先），所以地址 0 恒在网格原点（主卡 = 左上）。
 *
 *  合成而不是写一张常量表：单卡几何是**运行期**才知道的（dev_display_t 没有
 *  编译期几何宏，换模组只需改 board.mk 一行）—— 写死一张表，换屏就得同步改表，
 *  而不同步的表现是画面错位，不报错。
 *
 *  后续期这里改成"先查 W25Qxx 的切分表记录，没有才回落本网格" —— 那时才谈得上
 *  异形拼法与现场改址。 */
static screen_card_t   s_cards[SCREEN_CARD_MAX];
static screen_layout_t s_layout = {.cards = s_cards, .count = 0, .rows = 0, .cols = 0};

const screen_layout_t *app_screen_layout(void)
{
    return &s_layout;
}

const screen_card_t *app_screen_card(uint8_t card_idx)
{
    return (card_idx < s_layout.count) ? &s_layout.cards[card_idx] : nullptr;
}

uint8_t app_screen_index_of_addr(uint8_t addr)
{
    for (uint8_t i = 0; i < s_layout.count; i++)
        if (s_layout.cards[i].addr == addr) return i;
    return 0xFF;
}

uint8_t app_screen_self_index(void)
{
    return app_screen_index_of_addr(app_screen_self_addr());
}

uint16_t app_screen_card_bm_len(uint8_t card_idx)
{
    const screen_card_t *c = app_screen_card(card_idx);
    if (!c) return 0;
    return (uint16_t)(((c->w + 7U) / 8U) * c->h);
}

/** @brief 按 nx×ny 的网格合成切分表；本卡屏几何取自运行期的 display
 *
 *  取参数而不是直接读 board.h 的宏：后续期这里是"先查 W25Qxx 的切分表记录、
 *  没有才回落网格"的那条路，届时 nx/ny 来自记录。
 *
 *  @param master_cell 主卡所在的**网格下标**（行优先）。
 *
 *  **主卡不必在网格原点** —— 现场的拼法就有"上面一块、下面一块，下面那块是主卡"
 *  的（此时主卡在最后一行）。所以地址不能拿网格下标当：主卡那格编 0，
 *  其余按行优先依次编 1、2、3…。全工程没有任何地方假设主卡在原点
 *  （持久化恢复也按本卡矩形映射，见 _persist_restore）。 */
static bool _layout_build_grid(uint8_t nx, uint8_t ny, uint8_t master_cell)
{
    const uint16_t cw = s_display ? s_display->screen_rows : 0; /* 单卡屏宽 */
    const uint16_t ch = s_display ? s_display->screen_cols : 0; /* 单卡屏高 */

    if (!cw || !ch) return false;
    if ((uint16_t)nx * ny > SCREEN_CARD_MAX) {
        printf("[screen] 切分 %ux%u 张卡超过 SCREEN_CARD_MAX=%u\n", (unsigned)nx, (unsigned)ny,
               (unsigned)SCREEN_CARD_MAX);
        return false;
    }
    if (master_cell >= (uint8_t)(nx * ny)) {
        printf("[screen] 主卡格号 %u 超出 %ux%u 网格（0..%u）\n", (unsigned)master_cell,
               (unsigned)nx, (unsigned)ny, (unsigned)(nx * ny - 1));
        return false;
    }

    s_layout.count = (uint8_t)(nx * ny);
    s_layout.rows  = (uint16_t)(cw * nx); /* 整屏宽 */
    s_layout.cols  = (uint16_t)(ch * ny); /* 整屏高 */

    uint8_t next_addr = 1; /* 0 留给主卡 */
    for (uint8_t r = 0; r < ny; r++)
        for (uint8_t c = 0; c < nx; c++) {
            const uint8_t i = (uint8_t)(r * nx + c);
            s_cards[i]      = (screen_card_t){
                .addr  = (i == master_cell) ? 0U : next_addr++,
                .color = BOARD_SCREEN_COLOR,
                .x     = (uint16_t)(c * cw),
                .y     = (uint16_t)(r * ch),
                .w     = cw,
                .h     = ch,
            };
        }
    return true;
}

/** @brief 本期取板级网格参数（见 board.h 的 BOARD_CASCADE_COLS/ROWS/MASTER_CELL） */
static bool _layout_build(void)
{
    return _layout_build_grid((uint8_t)BOARD_CASCADE_COLS, (uint8_t)BOARD_CASCADE_ROWS,
                              (uint8_t)BOARD_CASCADE_MASTER_CELL);
}

/** @brief 由切分表推出画布几何、定位本卡、清画布
 *
 *  `_screen_init` 与 host 用例都走这一条 —— 用例换一组几何/网格时不必自己拼
 *  "设几何 + 清画布"那几步，也就不会与生产初始化漂移。
 *  @return false = 本卡地址不在表里，或画布池装不下整屏 */
static bool _apply_layout(void)
{
    s_rows   = s_layout.rows;
    s_cols   = s_layout.cols;
    s_stride = (uint16_t)((s_rows + 7U) / 8U);
    s_bm_len = (uint16_t)(s_stride * s_cols);

    s_self = app_screen_self_index();
    if (s_self >= s_layout.count) {
        /* 本卡地址不在切分表里 = 板上的地址与部署对不上。**不静默降级成"单卡占满"**：
           那会让现场以为一切正常，只是别的卡永远不亮（而"别的卡不亮"最容易被当成
           硬件故障去查线）。 */
        printf("[screen] 本卡地址 %u 不在切分表里（表内 %u 张卡），整屏门面停用\n",
               (unsigned)app_screen_self_addr(), (unsigned)s_layout.count);
        return false;
    }
    s_color = s_layout.cards[s_self].color;

#if BOARD_SCREEN_CANVAS
    /* 尺寸校验必须在 memset 之前 —— 池子小了先清就是直接写穿 */
    if (s_bm_len > sizeof(s_canvas)) {
        /* 拦下而不是截断：画布小了的表现是"右边/下边一块永远不更新"，很难查 */
        printf("[screen] 画布需要 %u 字节 > BOARD_SCREEN_CANVAS_MAX %u，整屏门面停用\n",
               (unsigned)s_bm_len, (unsigned)sizeof(s_canvas));
        return false;
    }
    memset(s_canvas, 0, s_bm_len);
#endif
    return true;
}

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

/* ================================================================
 *  抽带：画布上的一个矩形 → 一张 1bpp 位图
 *
 *  这是**主卡本地提交**与**发给从卡**共用的唯一提取路径 —— 两条路各写一份提取
 *  逻辑，迟早会在某个边界（w 不是 8 的倍数、x 不对齐）上漂移，而漂移的表现是
 *  "主卡屏上对、从卡屏上差一列"，现场几乎无法归因。
 * ================================================================ */

bool app_screen_extract(uint8_t card_idx, uint8_t *buf, uint16_t cap)
{
    const screen_card_t *c = app_screen_card(card_idx); /* **下标**，不是地址 */
    if (!c || !buf) return false;

    const uint16_t stride = (uint16_t)((c->w + 7U) / 8U);
    const uint16_t need   = (uint16_t)(stride * c->h);

    /* 矩形必须整个落在画布里。网格切分下恒真；切分表可由记录覆盖后就未必了，
       所以这里挡住而不是让它读到画布外面去。 */
    if (cap < need || (uint32_t)c->x + c->w > s_rows || (uint32_t)c->y + c->h > s_cols)
        return false;

    memset(buf, 0, need);

    for (uint16_t y = 0; y < c->h; y++) {
        uint8_t       *dst = &buf[(uint32_t)y * stride];
        const uint8_t *row = &s_canvas[(uint32_t)(c->y + y) * s_stride];

        if ((c->x & 7U) == 0U) {
            /* 矩形按字节对齐 —— 两板的卡宽都是 8 的倍数，这是常见情形（整行 memcpy） */
            memcpy(dst, &row[c->x >> 3], stride);
        } else {
            for (uint16_t x = 0; x < c->w; x++) {
                const uint16_t sx = (uint16_t)(c->x + x);
                if (row[sx >> 3] & (uint8_t)(0x80U >> (sx & 7U)))
                    dst[x >> 3] |= (uint8_t)(0x80U >> (x & 7U));
            }
        }

        /* 末字节补位归零：w 不是 8 的倍数时，memcpy 会把矩形右边**属于邻卡**的
           位也搬过来。不清掉的话同一幅画面会有两种字节表示（取决于画布右边是什么），
           比对与差分都失去意义。 */
        if (c->w & 7U) dst[stride - 1U] &= (uint8_t)(0xFFU << (8U - (c->w & 7U)));
    }
    return true;
}

bool app_screen_commit_self(void)
{
    if (s_self >= s_layout.count) return false;
    const uint16_t len = app_screen_card_bm_len(s_self);
    if (!len || len > sizeof(s_band)) return false;
    if (!app_screen_extract(s_self, s_band, sizeof(s_band))) return false;

    /* 走的是与从卡落屏完全相同的那个函数 —— 主从两侧的落屏行为逐字一致 */
    app_screen_commit_bitmap(s_band, len, s_color);
    return true;
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
    /* 复用 app_render 原有的落盘路径：它读的是 dev_display 的 pixel_map，而**本卡
       那一块**画布刚刚经 app_screen_commit_self 提交给它，两者逐位一致。
       所以存实屏 == 存本卡矩形，是多卡下也对的一件事（画布其余部分属于别的卡）。

       刻意**不**改成存整张画布：记录格式（render_persist_t）的尺寸域与位图上限
       （RENDER_PERSIST_BITMAP_MAX=2560）都是按**单块屏**定的，整屏画布 4 卡能到
       5600 字节，存不下。要存整屏得先改记录格式 —— 那是另一件事。 */
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
     *
     * 记录里存的是**本卡那块实屏**（render_persist_t 的尺寸域与位图上限都按单块屏
     * 算的），不是整屏画布。所以这里要把实屏按"本卡在画布上的矩形"摆回去 ——
     * 多卡时画布比实屏大，直接按画布尺寸索引 pixel_map 会读到屏外。
     * 索引一律用**实屏几何** dw/dh，与画布几何 s_rows/s_cols 是两回事。 */
    if (ok && s_display && s_self < s_layout.count) {
        const screen_card_t *c  = &s_layout.cards[s_self];
        const uint16_t       dw = s_display->screen_rows;
        const uint16_t       dh = s_display->screen_cols;

        memset(s_canvas, 0, s_bm_len);
        for (uint16_t y = 0; y < dh && y < c->h; y++)
            for (uint16_t x = 0; x < dw && x < c->w; x++)
                if (s_display->pixel_map[(uint32_t)y * dw + x] != COLOR_BLACK) {
                    const uint16_t cx = (uint16_t)(c->x + x);
                    const uint16_t cy = (uint16_t)(c->y + y);
                    s_canvas[(uint32_t)cy * s_stride + (cx >> 3)] |= (uint8_t)(0x80U >> (cx & 7U));
                }
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

bool app_screen_take_pending_settled(void)
{
    if (!s_pending) return false;
    if ((osKernelGetTickCount() - s_last_write_tick) < SCREEN_SETTLE_MS) return false;
    s_pending = false;
    return true;
}

static void _screen_task(void *arg)
{
    (void)arg;
    for (;;) {
        osDelay(SCREEN_POLL_MS);
        if (app_screen_take_pending_settled()) (void)app_screen_commit_self();
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

    if (!_layout_build()) {
        printf("[screen] 切分表合成失败，整屏门面停用\n");
        s_display = nullptr;
        return;
    }
    /* 逻辑几何 = **整屏**（多卡时比本卡那块屏大）—— 主卡画的就是这个 */
    if (!_apply_layout()) {
        s_display = nullptr;
        return;
    }

    /* board.h 的 BOARD_CASCADE_BAND_MAX 只是编译期上限，这里按**运行期**几何核一次 */
    const uint16_t band = app_screen_card_bm_len(s_self);
    if (band > BOARD_CASCADE_BAND_MAX) {
        printf("[screen] 本卡矩形位图 %u > BOARD_CASCADE_BAND_MAX %u，整屏门面停用\n",
               (unsigned)band, (unsigned)BOARD_CASCADE_BAND_MAX);
        s_display = nullptr;
        return;
    }

#if BOARD_SCREEN_CANVAS
    /* 渲染目标几何必须跟着**整屏**走 —— 排版/换行/居中判的是它 */
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

    const screen_card_t *c = &s_layout.cards[s_self];
    printf("[screen] 整屏画布 %ux%u（%u 字节，1bpp）共 %u 卡；本卡 #%u addr=%u "
           "矩形 %ux%u@(%u,%u) 颜色 %u\n",
           (unsigned)s_rows, (unsigned)s_cols, (unsigned)s_bm_len, (unsigned)s_layout.count,
           (unsigned)s_self, (unsigned)app_screen_self_addr(), (unsigned)c->w, (unsigned)c->h,
           (unsigned)c->x, (unsigned)c->y, (unsigned)s_color);
#else
    printf("[screen] 整屏门面未启用（BOARD_SCREEN_CANVAS=0），渲染直写实屏\n");
#endif
}
sw_dev_initcall(_screen_init);
