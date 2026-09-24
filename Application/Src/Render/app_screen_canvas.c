/**
 * @file    app_screen_canvas.c
 * @brief   整屏逻辑画布实现 —— 1bpp 画布、渲染目标、抽带、落屏与持久化
 *
 * 由 app_screen.c 拆出：整屏门面（几何/身份）留在 app_screen.c，
 * 画布这一簇 —— 1bpp 缓冲、渲染目标 sink、抽带、静默期提交、显存持久化 —— 收在这里。
 *
 * 由 board.h 的 BOARD_SCREEN_CANVAS 开关：多卡级联的板子打开（5006048 即如此），
 * 单独一块卡上，画布是纯开销 —— 它多占一块 1bpp 缓冲、多一次拷贝，还要把卡内多色
 * 塌缩成单色，却没有换来任何功能。中间靠 `app_render_set_target(NULL)` 即可完全回退。
 *
 * 几何（整屏宽高）由门面持有，这里只通过 `app_screen_rows()/app_screen_cols()` 读；
 * 本文件自己的 static 只用于画布缓冲、stride/长度与这一帧的颜色账。
 */

#include "app_screen.h"

/* ================================================================
 *  画布关闭（BOARD_SCREEN_CANVAS=0）：整 TU 不进构建，只留两个桩符号
 *
 *  原因：Application/Src/BIST/app_factory_test.c **无条件**调
 *  `app_screen_set_color_override()`，而该文件不分板编译 —— 没有下面这两个桩，
 *  关闭画布的那块板会链接报"被引用却未定义"。`app_screen_output_color` 同理：
 *  它虽只在有画布时被 commit_self 调，但保持符号对称、语义退化为"原样输出"。
 * ================================================================ */
#if BOARD_SCREEN_CANVAS

#include <stdio.h>
#include <string.h>
#include "board.h" /* BOARD_SCREEN_CANVAS_MAX / BOARD_CASC_BAND_MAX */
#include "app_render.h"
#include "dev_display.h"
#include "cmsis_os2.h"

/* ---- 画布 ----
 * 1bpp 字节池。静态定长、按"本板参与的最大级联规模"留，运行期几何从中切 ——
 * dev_display_t 没有编译期几何宏，尺寸只能运行期读（与本工程既有做法一致）。
 *
 * 放 SRAM 不放 CCMRAM：画布只在渲染与抽取时被碰（每轮几 KB），是冷路径，
 * 不值得跟协议 RB/队列（工程惯例放 CCMRAM）与显示缓冲抢那 64KB。
 * 且 CCMRAM 在 5006048 上只剩约 12KB，1B/px 的整屏画布根本放不下 ——
 * "整屏 1bpp"这个决策的真实价值就在这里。 */
static uint8_t s_canvas_buf[BOARD_SCREEN_CANVAS_MAX];

static uint16_t s_stride; /* = (整屏宽 + 7) / 8 */
static uint16_t s_bm_len; /* = s_stride * 整屏高 */

/* 抽带缓冲：本卡矩形抽出来放这儿，再交给 commit_bitmap 落屏。
   主卡本地提交与"单卡即整屏"共用它 —— 长度由 BOARD_CASC_BAND_MAX 兜底，
   attach 时按**运行期几何**校验一次（超了门面会明确打出来并停用）。 */
static uint8_t s_band_buf[BOARD_CASC_BAND_MAX];

/* ---- 输出颜色的临时覆盖（只给工厂老化测试用）----
 *
 * 根因：画布是 **1bpp**（只记亮/灭），**颜色在协议里是逐卡给的**
 * （`app_casc_image_t.color`，来自切分表）。所以"整屏轮流点亮红/绿/蓝"这种
 * 逐色老化，在级联下没有别的表达方式 —— 不覆盖的话所有纯色填充都会显示成
 * **本卡那个颜色**（现场：十种颜色全是绿的）。 */
#define SCREEN_COLOR_NO_OVERRIDE (0xFFU)
static uint8_t s_color_override = SCREEN_COLOR_NO_OVERRIDE;

void app_screen_set_color_override(uint8_t color)
{
    s_color_override = color;
}

/* ---- 这一帧内容用的颜色（画布只记亮/灭，颜色另行记着）----
 *
 * 画布是 **1bpp**，原来的规矩是"颜色由像素属于哪张卡决定" —— 于是 `app_render`
 * 传下来的颜色**被整个丢掉**：LDI 发红字显示成绿、RLS 的位图颜色同样中招、
 * 工厂老化逐色全绿。开画布之前（渲染直写实屏）颜色是逐像素的真彩，所以这是
 * 开画布之后新出现的回归。
 *
 * 现在的规矩：记下这一帧用了哪个**非黑**颜色，落屏时用它。
 *   · 只有一种非黑颜色（文字、位图、填充 —— 绝大多数用法）→ 就是它 ✓
 *   · 混用多种 → 退回"本卡颜色"，并置混色标志（1bpp 画布本来就表达不了多色，
 *     与其静默挑一个，不如退回那张卡的部署色）
 *   · 一次非黑都没写过（比如整屏全黑）→ 同样退回落卡片色 ✓ */
static uint8_t s_content_color = SCREEN_COLOR_NO_OVERRIDE; /* 0xFF = 本帧还没定 */
static bool    s_content_mixed;

/** @brief 这一次填充是不是"整屏清屏"（= 新一帧的开始）
 *
 *  各个渲染调用点的清屏都是"整屏黑填充"（`APP_RENDER_TYPE_FILL` 的 w=h=0）——
 *  它是唯一可靠的"上一帧结束"信号（画布本身被 memset 清只发生在重装门面时）。 */
static bool _is_full_clear(uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t c)
{
    return c == DEV_DISPLAY_COLOR_BLACK && x == 0 && y == 0 && w >= app_screen_rows() &&
           h >= app_screen_cols();
}

/** @brief 记下"这一帧用了哪个颜色"；只在写**亮**像素时调 */
static void _note_content_color(dev_display_color_t c)
{
    if (c == DEV_DISPLAY_COLOR_BLACK) return; /* 黑 = 灭，不算颜色 */
    if (s_content_color == SCREEN_COLOR_NO_OVERRIDE) {
        s_content_color = (uint8_t)c;
        return;
    }
    if (s_content_color != (uint8_t)c) s_content_mixed = true;
}

/** @brief 新一帧开始（画布被清）—— 上一帧的颜色主张作废 */
static void _reset_content_color(void)
{
    s_content_color = SCREEN_COLOR_NO_OVERRIDE;
    s_content_mixed = false;
}

uint8_t app_screen_output_color(uint8_t card_color)
{
    /* 优先级：工厂测试的强制覆盖 > 这一帧内容的颜色 > 切分表给这张卡的颜色 */
    if (s_color_override <= (uint8_t)DEV_DISPLAY_COLOR_WHITE) return s_color_override;
    if (!s_content_mixed && s_content_color <= (uint8_t)DEV_DISPLAY_COLOR_WHITE) return s_content_color;
    return card_color;
}

/** 本上电周期内画布**有没有被写过**（任何渲染）。
 *
 *  **`_persist_restore()` 直写画布、不经过 sink → 不置位** —— 这正是
 *  "上电恢复不算新内容"的干净表达。 */
static bool s_canvas_touched_flag;

bool app_screen_canvas_touched(void)
{
    return s_canvas_touched_flag;
}

/* ================================================================
 *  落屏静默期
 *
 *  为什么不"每次 app_render 返回就提交"：
 *   · `_vms_display_ctrl` 是"先 fill 再 bitmap"两次调用，逐次提交会推两遍整屏
 *   · 文字是**逐字** fill+draw_bitmap，中间态会被推出去（屏上会闪）
 *   · 调用点有六处，靠人记得 flush 迟早会漏
 *  静默 50ms 之后才提交，上述三类问题一次解决，且现有调用点一行都不用改。
 * ================================================================ */

static volatile uint32_t s_last_write_tick; /* 最后一次写入的时刻，静默期据此算 */
static volatile bool     s_pending_flag;    /* 有内容尚未落屏 */

#define SCREEN_SETTLE_MS (50U) /**< 静默多久算"这一屏画完了" */

/* ================================================================
 *  内部接缝实现
 * ================================================================ */

bool app_screen_canvas_attach(uint16_t rows, uint16_t cols)
{
    s_stride = (uint16_t)((rows + 7U) / 8U);
    s_bm_len = (uint16_t)(s_stride * cols);

    /* 尺寸校验必须在 memset 之前 —— 池子小了先清就是直接写穿 */
    if (s_bm_len > sizeof(s_canvas_buf)) {
        /* 拦下而不是截断：画布小了的表现是"右边/下边一块永远不更新"，很难查 */
        printf("[screen] 画布需要 %u 字节 > BOARD_SCREEN_CANVAS_MAX %u，整屏门面停用\n",
               (unsigned)s_bm_len, (unsigned)sizeof(s_canvas_buf));
        return false;
    }
    /* **每次重装都清**（换身份也一样）：换了身份之后画布上那份内容是不是
       "一整幅完整的逻辑屏"就说不准了 —— 从卡的画布只有它自己那一块（上电从记录
       恢复来的），把它当整幅推下去，别的卡当场被刷黑。而"按一下键屏上内容消失"
       只在**角色真的变了**的时候发生（没变的那条路在 apply_identity 就返回了）。 */
    memset(s_canvas_buf, 0, s_bm_len);
    _reset_content_color(); /* 上一帧的颜色主张作废 */
    return true;
}

/* ================================================================
 *  抽带：画布上的一个矩形 → 一张 1bpp 位图
 *
 *  这是**主卡本地提交**与**发给从卡**共用的唯一提取路径 —— 两条路各写一份提取
 *  逻辑，迟早会在某个边界（w 不是 8 的倍数、x 不对齐）上漂移，而漂移的表现是
 *  "主卡屏上对、从卡屏上差一列"，现场几乎无法归因。
 * ================================================================ */

bool app_screen_extract(uint8_t card_idx, uint8_t *buf, uint16_t cap)
{
    const app_screen_card_t *c = app_screen_card(card_idx); /* **下标**，不是地址 */
    if (!c || !buf) return false;

    const uint16_t stride = (uint16_t)((c->w + 7U) / 8U);
    const uint16_t need   = (uint16_t)(stride * c->h);

    /* 矩形必须整个落在画布里。网格切分下恒真；切分表可由记录覆盖后就未必了，
       所以这里挡住而不是让它读到画布外面去。 */
    if (cap < need || (uint32_t)c->x + c->w > app_screen_rows() ||
        (uint32_t)c->y + c->h > app_screen_cols())
        return false;

    memset(buf, 0, need);

    for (uint16_t y = 0; y < c->h; y++) {
        uint8_t       *dst = &buf[(uint32_t)y * stride];
        const uint8_t *row = &s_canvas_buf[(uint32_t)(c->y + y) * s_stride];

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

/* 前置声明：定义在文件后部（持久化那一节），而提交路径要用它 */
static void _persist_save(void);
static bool _persist_restore(void);

bool app_screen_commit_self(void)
{
    const uint8_t idx = app_screen_self_index();
    const app_screen_card_t *c = app_screen_card(idx);
    if (!c) return false;

    const uint16_t len = app_screen_card_bm_len(idx);
    if (!len || len > sizeof(s_band_buf)) return false;
    if (!app_screen_extract(idx, s_band_buf, sizeof(s_band_buf))) return false;

    /* 走的是与从卡落屏完全相同的那个函数 —— 主从两侧的落屏行为逐字一致。
       颜色过一道"输出颜色"：正常就是本卡那个颜色，工厂逐色老化时被临时覆盖。 */
    app_screen_commit_bitmap(s_band_buf, len, app_screen_output_color(c->color));

    /* ---- 持久化请求**只能在这里**消费 ----
     * 上面那一行刚把内容写进实屏，此刻存下去才是这一帧。渲染时（app_render）存的话
     * 读到的还是上一帧 —— 那是加这条缝之前三个调用点的缺陷。
     * 单卡走 _screen_task → 本函数；多卡主卡走 _round_run → 本函数 —— 两条都覆盖。 */
    if (app_render_take_persist_req()) _persist_save();
    return true;
}

/* ================================================================
 *  渲染目标实现
 *
 *  **边界裁剪自己做**：`dev_display_fill` / `dev_display_draw_bitmap` 现已各自
 *  自防御（越界一律裁剪，不再下溢、不再整张放弃），但画布这里**仍保留**自己的
 *  `_clip` —— 契约要求**目标层与 render 层各自防御**，任一层都不许指望对方兜底：
 *  设备层的修复不构成画布免除自身裁剪的理由。
 * ================================================================ */

static void _mark_dirty(void)
{
    s_last_write_tick    = osKernelGetTickCount();
    s_pending_flag       = true;
    s_canvas_touched_flag = true;
}

/** @brief 把矩形裁到画布内；全裁掉返回 false */
static bool _clip(uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    const uint16_t rows = app_screen_rows();
    const uint16_t cols = app_screen_cols();
    if (*x >= rows || *y >= cols) return false;
    if ((uint32_t)*x + *w > rows) *w = (uint16_t)(rows - *x);
    if ((uint32_t)*y + *h > cols) *h = (uint16_t)(cols - *y);
    return *w && *h;
}

/** @brief 置/清一个像素位 */
static inline void _set_bit(uint16_t x, uint16_t y, bool on)
{
    uint8_t *p   = &s_canvas_buf[(uint32_t)y * s_stride + (x >> 3)];
    uint8_t  msk = (uint8_t)(0x80U >> (x & 7U));
    if (on)
        *p |= msk;
    else
        *p &= (uint8_t)~msk;
}

/* 热路径：sink 开头把 stride 取进局部变量，逐像素循环只碰画布自己的缓冲与局部量。
   `app_screen_rows()` 只在这里读一次（几何已由 attach 固定，循环内不会变）。 */
static void _sink_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t c)
{
    (void)ctx;
    if (!_clip(&x, &y, &w, &h)) return;

    /* 画布只记亮/灭；具体是哪个非黑颜色由 `_note_content_color` 记着，
       落屏时用它（见 app_screen_output_color） */
    bool on = (c != DEV_DISPLAY_COLOR_BLACK);
    /* 整屏清屏 → 新一帧开始，上一帧的颜色主张作废；否则按颜色记账 */
    if (_is_full_clear(x, y, w, h, c)) _reset_content_color();
    else _note_content_color(c);

    const uint16_t stride = (uint16_t)((app_screen_rows() + 7U) / 8U);
    for (uint16_t r = 0; r < h; r++) {
        uint8_t *row = &s_canvas_buf[(uint32_t)(y + r) * stride];
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
                         const uint8_t *bm, dev_display_color_t c)
{
    (void)ctx;
    if (!bm) return;

    /* 源 stride 用**裁剪前**的 w 算：`_clip` 只砍可见列，不重排源位图的行字节。
       若先裁再算，行偏移会按裁剪后的宽度走 —— 第 2 行起整体错位（右边缘花屏）。 */
    const uint16_t src_stride = (uint16_t)((w + 7U) / 8U);

    if (!_clip(&x, &y, &w, &h)) return;

    /* 与 dev_display_draw_bitmap 同语义：bit=1 才写（写 on 或 off），bit=0 不动。
       位序同为 MSB-first、(宽+7)/8 行字节。 */
    bool on = (c != DEV_DISPLAY_COLOR_BLACK);
    _note_content_color(c);

    /* 逐像素经内联的 `_set_bit`（它用的 s_stride 是本 TU 的静态量，无跨 TU 调用） */
    for (uint16_t r = 0; r < h; r++) {
        for (uint16_t k = 0; k < w; k++) {
            if (bm[(uint32_t)r * src_stride + (k >> 3)] & (uint8_t)(0x80U >> (k & 7U)))
                _set_bit((uint16_t)(x + k), (uint16_t)(y + r), on);
        }
    }
    _mark_dirty();
}

/* ---- 渲染目标与持久化钩子（只在 BOARD_SCREEN_CANVAS 打开时注册）---- */

/* **不能加 const**：几何在 enable 里填，而 const 对象住 .rodata ——
   在 STM32 上那就是 Flash，写进去会被静默丢弃（不报错！），于是 rows/cols 恒为 0，
   画布裁剪把所有内容裁光、屏上什么都不显示。host 单测用 ASan 抓到的就是这个写。 */
static app_render_target_t s_target = {
    .fill   = _sink_fill,
    .bitmap = _sink_bitmap,
    .ctx    = nullptr,
    .rows   = 0, /* enable 里填 */
    .cols   = 0,
};

static const app_render_persist_hook_fn_t s_persist_hook = {.save = _persist_save, .restore = _persist_restore};

/* ---- 接缝：装/卸渲染目标与持久化钩子（定义在 s_target / s_persist_hook 之后）---- */

void app_screen_canvas_enable(uint16_t rows, uint16_t cols)
{
    /* 渲染目标几何必须跟着**整屏**走 —— 排版/换行/居中判的是它 */
    s_target.rows = rows;
    s_target.cols = cols;
    app_render_set_persist_hook(&s_persist_hook);
    app_render_set_target(&s_target);

    /* 画布刚清空：**待落屏必须清掉**（置着的话主卡会立刻把一张全黑推给所有从卡），
       "本周期被渲染过"这个闸也重新计 —— 新身份下这份内容要么没了、要么不完整。 */
    s_pending_flag        = false;
    s_canvas_touched_flag = false;
}

void app_screen_canvas_disable(void)
{
    app_render_set_persist_hook(nullptr);
    app_render_set_target(nullptr);

    s_pending_flag        = false;
    s_canvas_touched_flag = false;
}

/* ================================================================
 *  显存持久化：画布版本的存/取
 *
 *  接管之后存的**不再是实屏**，而是画布 —— 级联下这才是"整屏的内容"。
 *  格式沿用 app_render_persist_t（1bpp、MSB-first），但多带一个颜色字段已经够用
 *  （本卡颜色由切分表给，不随内容变）。
 *
 *  画布已大于本屏，存画布与存实屏不再等价；改动的意义在于接上钩子这条缝。
 * ================================================================ */

static void _persist_save(void)
{
    /* 复用 app_render 原有的落盘路径：它读的是 dev_display 的 pixel_map，而**本卡
       那一块**画布刚刚经 app_screen_commit_self 提交给它，两者逐位一致。
       所以存实屏 == 存本卡矩形，是多卡下也对的一件事（画布其余部分属于别的卡）。

       刻意**不**改成存整张画布：记录格式（app_render_persist_t）的尺寸域与位图上限
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
     * 记录里存的是**本卡那块实屏**（app_render_persist_t 的尺寸域与位图上限都按单块屏
     * 算的），不是整屏画布。所以这里要把实屏按"本卡在画布上的矩形"摆回去 ——
     * 多卡时画布比实屏大，直接按画布尺寸索引 pixel_map 会读到屏外。
     * 索引一律用**实屏几何** dw/dh，与画布几何 app_screen_rows()/cols() 是两回事。 */
    const dev_display_t      *d   = dev_display_get();
    const app_screen_card_t  *c   = app_screen_card(app_screen_self_index());
    if (ok && d && c) {
        const uint16_t dw = d->screen_rows;
        const uint16_t dh = d->screen_cols;

        memset(s_canvas_buf, 0, s_bm_len);
        /* 恢复的内容**直写画布**、不经过 sink —— 内容色在这里补记：
           实屏上那份是 `app_render_restore` 用**记录里的颜色**画出来的
           （存的时候也是取"第一个非黑像素的颜色"，见 app_render_save），
           所以照着像素记一遍即可，落屏时才不会退回卡片色。 */
        _reset_content_color();
        for (uint16_t y = 0; y < dh && y < c->h; y++)
            for (uint16_t x = 0; x < dw && x < c->w; x++) {
                const uint8_t px = d->pixel_map[(uint32_t)y * dw + x];
                if (px == DEV_DISPLAY_COLOR_BLACK) continue;

                _note_content_color((dev_display_color_t)px);
                const uint16_t cx = (uint16_t)(c->x + x);
                const uint16_t cy = (uint16_t)(c->y + y);
                s_canvas_buf[(uint32_t)cy * s_stride + (cx >> 3)] |= (uint8_t)(0x80U >> (cx & 7U));
            }
        s_pending_flag = false; /* 刚恢复的内容已经落过屏，不必再提交一遍 */
    }
    return ok;
}

/* ================================================================
 *  显式提交与静默期消费
 * ================================================================ */

void app_screen_flush(void)
{
    /* 把"最后一次写入"往前推过静默窗，下一次轮询就提交 */
    s_last_write_tick = osKernelGetTickCount() - SCREEN_SETTLE_MS;
    s_pending_flag    = true;
}

bool app_screen_take_pending_settled(void)
{
    if (!s_pending_flag) return false;
    if ((osKernelGetTickCount() - s_last_write_tick) < SCREEN_SETTLE_MS) return false;
    s_pending_flag = false;
    return true;
}

#else /* !BOARD_SCREEN_CANVAS */

/* 整 TU 仍产出下面两个符号，杜绝"被引用却未定义"：
   app_factory_test.c 无条件调 set_color_override，且不分板编译。
   两个函数是退化语义：不覆盖（谁调都当没调）、输出色原样返回卡片色。 */
void app_screen_set_color_override(uint8_t c)
{
    (void)c;
}

uint8_t app_screen_output_color(uint8_t card_color)
{
    return card_color;
}

#endif /* BOARD_SCREEN_CANVAS */
