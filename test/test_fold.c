/**
 * @file    test_fold.c
 * @brief   折叠屏：模式判定与变体1（E9 两行各限上/下半屏）
 *
 * **为什么需要这个测试**：折叠屏的两半是两块物理卡，内容分别落在两台设备上，
 * 上/下对调、或把两行都塞进上半，靠人眼在整屏上很难归因；而 `app_fold_split_lines`
 * 只给指针、不改输入，越界/少切一个字节也不报错，只是显示成错的内容。
 *
 * **打桩方式**：`app_vms_ctrl.c` 是 TU-include（`_vms_display_ctrl` 是 static），
 * `app_render` 换成套件内的 capture 桩，把每次渲染的 `type/x/y/w/h/text/len/style/bitmap`
 * 原样记下；`app_screen_*` 用可调桩。`app_fold.c` 与 `app_ldi.c` 作为普通 TU 编译
 * （后者提供真实的 `g_ldi_ctx` 与 `app_ldi_get_device_idx`，故模式判定的三态走的是
 * 真实访问器，而不是复制的判定逻辑）。
 *
 * **本片的范围**：变体1/变体2 的 E9 侧（变体2 只写/只清上半屏），以及变体2 的 EA
 * 数据面（`app_fold_preset_show` / `app_fold_lower_clear`，含尺寸/空槽/非法颜色拒画）。
 * EA 与 1BH 分派层的接线（CtlStatus 落值）在 test_ldi_0ah.c（那边 TU-include 了
 * app_ldi_cmd.c，不引入符号冲突）。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_ldi.h"
#include "app_ldi_cmd.h"
#include "app_fold.h"
#include "app_render.h"
#include "app_screen.h"
#include "dev_display.h" /* dev_display_t / dev_display_frame_begin/end 的桩要用到类型 */

/* ================================================================
 *  可调桩：整屏门面 + 光传感器
 * ================================================================ */

static uint8_t  s_fold_count = 1;   /**< app_screen_fold_count() 的返回 */
static uint16_t s_scr_w      = 128; /**< 逻辑屏宽（= app_screen_rows） */
static uint16_t s_scr_h      = 32;  /**< 逻辑屏高（= app_screen_cols） */

uint8_t app_screen_fold_count(void)
{
    return s_fold_count;
}

bool app_screen_fold_rect(uint8_t half, uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    if (s_fold_count != 2U) return false; /* 非折叠：一个输出参数都不写 */

    const uint16_t half_h = (uint16_t)(s_scr_h / 2U);
    const uint16_t yy     = (half == 0U) ? 0U : half_h;
    const uint16_t hh     = (half == 0U) ? half_h : (uint16_t)(s_scr_h - half_h);
    if (x) *x = 0;
    if (y) *y = yy;
    if (w) *w = s_scr_w;
    if (h) *h = hh;
    return true;
}

uint16_t app_screen_rows(void) { return s_scr_w; }
uint16_t app_screen_cols(void) { return s_scr_h; }

void app_light_sensor_resume(void) {}
void app_light_sensor_set_fixed(uint8_t level) { (void)level; }

/* ================================================================
 *  板级折叠预置图：本套件自己提供 g_board_fold_presets 的定义
 *
 *  同 g_board_font_lib 在 test_render 的做法 —— 板级数据不参与 host 编译，
 *  用例给一份合成的。绿/红两槽有效（不同缓冲，用来验证"取到对应槽"），
 *  黄槽空槽 —— 与 5006048 交付时"数据待补"的现状一致，用来钉住空槽拒画。
 *  尺寸 224×50 与用例里的下半屏一致；尺寸不符的用例通过**改布局**制造。
 * ================================================================ */
static uint8_t s_bm_green[224 * 50 / 8];
static uint8_t s_bm_red[224 * 50 / 8];

const app_fold_preset_t g_board_fold_presets[3] = {
    {.w = 224, .h = 50, .bitmap = s_bm_green}, /* [0] 绿 */
    {.w = 224, .h = 50, .bitmap = s_bm_red},   /* [1] 红 */
    {.w = 0, .h = 0, .bitmap = nullptr},       /* [2] 黄：空槽（数据待补） */
};

/* ================================================================
 *  板级字库：本套件自己提供 g_board_font_lib 的定义
 *
 *  同 test_render 的做法 —— 板级数据不参与 host 编译。app_vms_ctrl.c 的
 *  `_map_font_size` 按 g_board_font_lib.sizes[] 升序落档，故这里给一份最小合成
 *  （升序 16/32，无单元；本套件 TU-include app_vms_ctrl.c，不编 app_render.c，
 *  单元只是"找得到三元组"用的，此处用不到）。
 * ================================================================ */
static const app_font_size_t s_sizes[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_32};

const app_font_lib_desc_t g_board_font_lib = {
    .lib            = nullptr,
    .lib_count      = 0,
    .sizes          = s_sizes,
    .size_count     = sizeof(s_sizes) / sizeof(s_sizes[0]),
    .asc_index_base = 0x20,
    .gb_index       = APP_FONT_IDX_KIND_GB2312,
    .total_bytes    = 0,
};

/* ================================================================
 *  capture 渲染目标 —— 记录每次 app_render 的入参
 * ================================================================ */

typedef struct {
    app_render_type_t  type;
    uint16_t           x, y, w, h;
    dev_display_color_t color;
    const char        *text;
    uint16_t           len;
    app_font_size_t    font_size;
    app_render_style_t style;
    bool               has_style;
    bool               persist;
    const uint8_t     *bitmap; /**< BITMAP 源指针（其余类型为 nullptr） */
} render_rec_t;

#define REC_MAX (16)
static render_rec_t s_recs[REC_MAX];
static int          s_rec_cnt;

/* ---- 帧压制事件序列 ----
 *
 * "清 + 画包成一帧"不能只数渲染条数：还要确认 begin 在两次渲染之前、end 在两次渲染
 * 之后，且恰好各一次。捕获桩把 begin/end 也记进按调用先后排列的事件序列，断言
 * begin → FILL → BITMAP → end。 */
typedef enum {
    EV_FRAME_BEGIN,
    EV_FRAME_END,
    EV_RENDER,
} ev_kind_t;

#define EV_MAX (32)
static ev_kind_t s_ev[EV_MAX];
static int       s_ev_cnt;
static int       s_frame_begins; /**< dev_display_frame_begin 调用次数 */
static int       s_frame_ends;   /**< dev_display_frame_end 调用次数 */

static void ev_push(ev_kind_t k)
{
    if (s_ev_cnt < EV_MAX) s_ev[s_ev_cnt++] = k;
}

void app_render(const app_render_cfg_t *cfg)
{
    if (s_rec_cnt >= REC_MAX) return;
    ev_push(EV_RENDER);
    render_rec_t *r = &s_recs[s_rec_cnt++];
    memset(r, 0, sizeof(*r));
    r->type    = cfg->type;
    r->x       = cfg->x;
    r->y       = cfg->y;
    r->w       = cfg->w;
    r->h       = cfg->h;
    r->color   = cfg->color;
    r->persist = cfg->persist;
    if (cfg->type == APP_RENDER_TYPE_TEXT) {
        r->text      = cfg->text;
        r->len       = cfg->len;
        r->font_size = cfg->font_size;
        if (cfg->style) {
            r->style     = *cfg->style;
            r->has_style = true;
        }
    } else if (cfg->type == APP_RENDER_TYPE_BITMAP) {
        r->bitmap = cfg->bitmap;
    }
}

/* ---- dev_display 帧接口的捕获桩 ----
 *
 * app_fold.c 是普通 TU，它对 dev_display_frame_begin/end 的调用要由本套件解析。
 * `dev_display_get()` 只被透传给 begin/end，本套件不测落屏，返回 nullptr 即可
 * （stub 里 begin/end 不 deref dev）。 */
dev_display_t *dev_display_get(void)
{
    return nullptr;
}

void dev_display_frame_begin(dev_display_t *dev)
{
    (void)dev;
    s_frame_begins++;
    ev_push(EV_FRAME_BEGIN);
}

void dev_display_frame_end(dev_display_t *dev)
{
    (void)dev;
    s_frame_ends++;
    ev_push(EV_FRAME_END);
}

static int count_type(app_render_type_t t)
{
    int n = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].type == t) n++;
    return n;
}

static const render_rec_t *nth_text(int n)
{
    int k = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].type == APP_RENDER_TYPE_TEXT && k++ == n) return &s_recs[i];
    return nullptr;
}

static const render_rec_t *nth_type(app_render_type_t t, int n)
{
    int k = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].type == t && k++ == n) return &s_recs[i];
    return nullptr;
}

/* ================================================================
 *  被测：生产源码本体
 * ================================================================ */
#include "../Application/Src/LDI/app_vms_ctrl.c"

/* ================================================================
 *  断言
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

/** @brief 断言"这段被恰好一帧包住"：begin → renders 笔渲染 → end，且 begin/end 各一次 */
static void check_one_frame(int renders)
{
    CHECK_MSG(s_frame_begins == 1 && s_frame_ends == 1,
              "应为恰好一帧（begin/end 各一次），得到 begin=%d end=%d", s_frame_begins,
              s_frame_ends);
    CHECK_MSG(s_ev_cnt == renders + 2, "事件数应为 2 + %d 笔渲染 = %d，得到 %d", renders, renders + 2,
              s_ev_cnt);
    CHECK_MSG(s_ev_cnt >= 1 && s_ev[0] == EV_FRAME_BEGIN, "首事件必须是 begin");
    CHECK_MSG(s_ev_cnt >= 2 && s_ev[s_ev_cnt - 1] == EV_FRAME_END, "末事件必须是 end");
    for (int i = 1; i + 1 < s_ev_cnt; i++)
        CHECK_MSG(s_ev[i] == EV_RENDER, "中间第 %d 个事件必须是一笔渲染", i);
}

/* ================================================================
 *  夹具
 * ================================================================ */

/** @brief 复位夹具：默认 = 非折叠 + 逻辑屏 128×32 + 仅声明 E9 */
static void env_reset(void)
{
    s_rec_cnt      = 0;
    s_ev_cnt       = 0;
    s_frame_begins = 0;
    s_frame_ends   = 0;
    s_fold_count   = 1;
    s_scr_w        = 128;
    s_scr_h        = 32;

    /* 真实 g_ldi_ctx（app_ldi.c）的默认形态：module_count==2 但 modules[1] 是空槽 */
    g_ldi_ctx.cfg.module_count = 2;
    memset(g_ldi_ctx.cfg.modules, 0, sizeof(g_ldi_ctx.cfg.modules));
    g_ldi_ctx.cfg.modules[0].device_type  = APP_LDI_DEVICE_VMS;
    g_ldi_ctx.cfg.modules[0].device_index = 1;
}

/** @brief 追加声明一块 EA（雨棚信号灯）模块 */
static void declare_ea(void)
{
    g_ldi_ctx.cfg.modules[1].device_type  = APP_LDI_DEVICE_CANOPY_LIGHT;
    g_ldi_ctx.cfg.modules[1].device_index = 1;
}

/** @brief 构造一条 E9 显示控制（func 01H）帧，返回可传给 app_vms_ctrl 的上下文 */
static uint8_t s_vms_buf[128];

static app_ldi_ctrl_vms_t *make_vms(const char *text, uint16_t len, uint8_t format, uint8_t font_line,
                                    uint8_t keep_time, uint8_t font_size)
{
    memset(s_vms_buf, 0, sizeof(s_vms_buf));
    app_ldi_ctrl_vms_t *ctx = (app_ldi_ctrl_vms_t *)s_vms_buf;
    ctx->device_func_type = 0x01;
    ctx->font_color       = 2; /* 红 */
    ctx->font_size        = font_size;
    ctx->font_line        = font_line;
    ctx->keep_time        = keep_time;
    ctx->format           = format;
    memcpy(ctx->text, text, len);
    return ctx;
}

/* ================================================================
 *  用例
 * ================================================================ */

/** 三态判定：几何门禁优先；EA 的判据是"声明了模块"而非 module_count */
static void case_mode(void)
{
    TEST_BEGIN("模式判定：非折叠→FLAT；折叠+仅 E9→FOLD_E9；折叠+E9+EA→FOLD_E9_EA");

    /* ① 非折叠：无论声明什么，都 FLAT（3833024 的保命线） */
    env_reset();
    CHECK_MSG(app_fold_mode() == APP_FOLD_MODE_FLAT, "非折叠应判 FLAT，得到 %u",
              (unsigned)app_fold_mode());
    declare_ea(); /* 非折叠 + 声明 EA 仍是 FLAT */
    CHECK_MSG(app_fold_mode() == APP_FOLD_MODE_FLAT, "非折叠即使声明 EA 也应 FLAT，得到 %u",
              (unsigned)app_fold_mode());

    /* ② 折叠 + 默认配置（module_count==2 但只有一个真模块）→ FOLD_E9。
          这条同时钉住"不能数 module_count"：数数量会把默认配置误判成变体2。 */
    env_reset();
    s_fold_count = 2;
    CHECK_MSG(app_fold_mode() == APP_FOLD_MODE_FOLD_E9,
              "折叠 + 默认配置（modules[1] 为空槽）应 FOLD_E9，得到 %u", (unsigned)app_fold_mode());

    /* ③ 折叠 + 声明 EA → FOLD_E9_EA */
    env_reset();
    s_fold_count = 2;
    declare_ea();
    CHECK_MSG(app_fold_mode() == APP_FOLD_MODE_FOLD_E9_EA, "折叠 + 声明 EA 应 FOLD_E9_EA，得到 %u",
              (unsigned)app_fold_mode());
}

/** app_fold_rect 薄封装：转发给 app_screen_fold_rect，非折叠时一个输出都不写 */
static void case_rect(void)
{
    TEST_BEGIN("app_fold_rect：折叠时转发上/下半屏；非折叠返回 false 且不写输出");

    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;

    uint16_t x = 0, y = 0, w = 0, h = 0;
    CHECK_MSG(app_fold_rect(0, &x, &y, &w, &h), "上半矩形应取到");
    CHECK_MSG(x == 0 && y == 0 && w == 224 && h == 50, "上半应为 224x50@(0,0)，得到 %ux%u@(%u,%u)",
              (unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y);
    CHECK_MSG(app_fold_rect(1, &x, &y, &w, &h), "下半矩形应取到");
    CHECK_MSG(x == 0 && y == 50 && w == 224 && h == 50, "下半应为 224x50@(0,50)，得到 %ux%u@(%u,%u)",
              (unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y);

    s_fold_count = 1;
    x = 0xDEAD, y = 0xBEEF, w = 0xCAFE, h = 0xF00D;
    CHECK_MSG(!app_fold_rect(0, &x, &y, &w, &h), "非折叠时 app_fold_rect 应返回 false");
    CHECK_MSG(x == 0xDEAD && y == 0xBEEF && w == 0xCAFE && h == 0xF00D,
              "非折叠时不得写输出参数（得到 %04X %04X %04X %04X）", (unsigned)x, (unsigned)y,
              (unsigned)w, (unsigned)h);
}

/** 按 \n 切两段：行数含空段、只给指针不改输入、多余行由调用方丢 */
static void case_split_lines(void)
{
    TEST_BEGIN("app_fold_split_lines：行数含空段、只给指针不改输入");

    const char *l0 = nullptr, *l1 = nullptr;
    uint16_t    n0 = 0, n1 = 0;

    /* ① 两行 */
    const char *t2 = "AB\nCD";
    CHECK_MSG(app_fold_split_lines(t2, 5, &l0, &n0, &l1, &n1) == 2, "两行文本应返回 2");
    CHECK_MSG(l0 == t2 && n0 == 2 && l1 == t2 + 3 && n1 == 2, "两段应为 AB(2) / CD(2)");

    /* ② 一行：没有 \n，第 2 段为空段 */
    const char *t1 = "AB";
    CHECK_MSG(app_fold_split_lines(t1, 2, &l0, &n0, &l1, &n1) == 1, "一行文本应返回 1");
    CHECK_MSG(l0 == t1 && n0 == 2 && n1 == 0, "第 1 段应为整段 AB，第 2 段为空");

    /* ③ 三行：line1 只到第 2 个 \n（多余内容由调用方丢弃） */
    const char *t3  = "A\nBB\nCCC";
    char        cp3[sizeof("A\nBB\nCCC")]; /* 含结尾 NUL，按它整份比对 */
    memcpy(cp3, t3, sizeof(cp3));
    CHECK_MSG(app_fold_split_lines(t3, 8, &l0, &n0, &l1, &n1) == 3, "三行文本应返回 3");
    CHECK_MSG(l0 == t3 && n0 == 1 && l1 == t3 + 2 && n1 == 2,
              "第 1/2 段应为 A(1) / BB(2)（不跨到第 3 行）");
    CHECK_MSG(memcmp(cp3, t3, sizeof(cp3)) == 0, "本函数必须不修改输入文本");

    /* ④ 结尾换行 → 空段也算一行 */
    const char *t4 = "A\n";
    CHECK_MSG(app_fold_split_lines(t4, 2, &l0, &n0, &l1, &n1) == 2, "结尾 \\n 应切出 2 行（末段为空）");
    CHECK_MSG(n0 == 1 && n1 == 0, "应为 A(1) / 空(0)，得到 %u / %u", (unsigned)n0, (unsigned)n1);

    /* ⑤ 空文本 → 1 行空段（0 段也算行） */
    CHECK_MSG(app_fold_split_lines("", 0, &l0, &n0, &l1, &n1) == 1, "空文本应返回 1 行");
    CHECK_MSG(n0 == 0 && n1 == 0, "空文本两段长度都应为 0");

    /* ⑥ 输出参数可传 nullptr（调用方只要行数） */
    CHECK_MSG(app_fold_split_lines(t2, 5, nullptr, nullptr, nullptr, nullptr) == 2,
              "输出参数全为 nullptr 时仍应返回行数");
}

/** 变体1 · 2 行：上/下半屏各一次渲染，文本片段正确、样式与 font_line 忽略 */
static void case_variant1_two_lines(void)
{
    TEST_BEGIN("变体1 · 2 行：上/下半屏各一次渲染，'_'→换行、font_line 被忽略");

    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;

    /* font_line=2 在旧语义里会把内容放到 y=(2-1)*16=16 那一条带；折叠模式必须忽略它 */
    app_ldi_ctrl_vms_t *ctx = make_vms("AB_CD", 5, /*format=*/1, /*font_line=*/2, /*keep=*/0,
                                       /*font_size=*/0);
    app_vms_ctrl(ctx, 5);

    /* 清屏 + 上/下两笔文字**被同一帧包住**：begin → FILL → TEXT → TEXT → end */
    check_one_frame(3);

    /* 整屏清一次 + 上/下半屏各一次 = 3 次 */
    CHECK_MSG(s_rec_cnt == 3, "应为 1 清屏 + 2 文本（3 次），得到 %d", s_rec_cnt);
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "应恰好清屏一次，得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(count_type(APP_RENDER_TYPE_TEXT) == 2, "应恰好渲染 2 次文本，得到 %d",
              count_type(APP_RENDER_TYPE_TEXT));

    const render_rec_t *top = nth_text(0);
    const render_rec_t *bot = nth_text(1);

    CHECK_MSG(top && top->x == 0 && top->y == 0 && top->w == 224 && top->h == 50,
              "上半区域应为 224x50@(0,0)（font_line 被忽略），得到 %ux%u@(%u,%u)",
              top ? (unsigned)top->w : 0U, top ? (unsigned)top->h : 0U, top ? (unsigned)top->x : 0U,
              top ? (unsigned)top->y : 0U);
    CHECK_MSG(bot && bot->x == 0 && bot->y == 50 && bot->w == 224 && bot->h == 50,
              "下半区域应为 224x50@(0,50)，得到 %ux%u@(%u,%u)", bot ? (unsigned)bot->w : 0U,
              bot ? (unsigned)bot->h : 0U, bot ? (unsigned)bot->x : 0U, bot ? (unsigned)bot->y : 0U);

    CHECK_MSG(top && top->len == 2 && top->text[0] == 'A' && top->text[1] == 'B',
              "上半文本应为 'AB'（'_' 已转 '\\n'）");
    CHECK_MSG(bot && bot->len == 2 && bot->text[0] == 'C' && bot->text[1] == 'D',
              "下半文本应为 'CD'");

    CHECK_MSG(top && top->has_style && top->style.word_wrap && top->style.prefer_one_line,
              "折叠样式应开 word_wrap + prefer_one_line");
    CHECK_MSG(top && top->style.v_align == APP_RENDER_ALIGN_CENTER, "折叠样式 v_align 应为居中");
    CHECK_MSG(top && top->style.h_align == APP_RENDER_ALIGN_CENTER,
              "h_align 应沿用协议 format=01H（居中），得到 %u", top ? (unsigned)top->style.h_align : 99U);
    CHECK_MSG(bot && bot->has_style && bot->style.word_wrap && bot->style.prefer_one_line,
              "下半样式应与上半一致");

    /* keep_time==0 → 两半都请求落盘（持久化粒度 = 半屏 = 本卡矩形，语义不变） */
    CHECK_MSG(top && top->persist && bot && bot->persist, "keep_time==0 时两半都应请求落盘");
}

/** 变体1 · 1 行：只画上半，下半保持清空（只有一次文本渲染） */
static void case_variant1_one_line(void)
{
    TEST_BEGIN("变体1 · 1 行：只渲染上半屏（下半不渲染、保持清空）");

    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;

    /* keep_time=5 → 不落盘 */
    app_ldi_ctrl_vms_t *ctx = make_vms("AB", 2, 1, 0, 5, 0);
    app_vms_ctrl(ctx, 2);

    /* 1 行只画上半：仍是 begin → 清 → 上半文字 → end（不是"提前 return 漏 end"） */
    check_one_frame(2);

    CHECK_MSG(s_rec_cnt == 2, "应为 1 清屏 + 1 文本（2 次），得到 %d", s_rec_cnt);
    CHECK_MSG(count_type(APP_RENDER_TYPE_TEXT) == 1, "只应渲染上半：文本恰好 1 次，得到 %d",
              count_type(APP_RENDER_TYPE_TEXT));

    const render_rec_t *top = nth_text(0);
    CHECK_MSG(top && top->x == 0 && top->y == 0 && top->w == 224 && top->h == 50,
              "1 行时应落在上半屏 224x50@(0,0)");
    CHECK_MSG(top && top->len == 2 && top->text[0] == 'A', "上半文本应为 'AB'");
    CHECK_MSG(top && !top->persist, "keep_time!=0 时不该请求落盘");
}

/** 变体1 · >2 行：只取前 2 行，第 3 行不渲染 */
static void case_variant1_three_lines(void)
{
    TEST_BEGIN("变体1 · 3 行：只取前 2 行（第 3 行不渲染）");

    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;

    app_ldi_ctrl_vms_t *ctx = make_vms("A_B_C", 5, 1, 0, 0, 0);
    app_vms_ctrl(ctx, 5);

    /* >2 行只取前 2 行（第 3 行丢弃）—— 丢弃是中间的一条日志，不是提前 return，
       整段仍在同一帧内：begin → FILL → TEXT → TEXT → end */
    check_one_frame(3);

    CHECK_MSG(s_rec_cnt == 3, "应为 1 清屏 + 前 2 行（3 次），得到 %d", s_rec_cnt);
    CHECK_MSG(count_type(APP_RENDER_TYPE_TEXT) == 2, "必须恰好 2 次文本（第 3 行被丢弃），得到 %d",
              count_type(APP_RENDER_TYPE_TEXT));

    const render_rec_t *top = nth_text(0);
    const render_rec_t *bot = nth_text(1);
    CHECK_MSG(top && top->len == 1 && top->text[0] == 'A', "上半应为 'A'");
    CHECK_MSG(bot && bot->len == 1 && bot->text[0] == 'B', "下半应为 'B'（第 3 段 'C' 被丢弃）");
}

/** FLAT：与折叠引入前逐字一致 —— 单次渲染、区域 = 逻辑屏（font_line 照旧生效） */
static void case_flat(void)
{
    TEST_BEGIN("FLAT：单次渲染、区域 = 逻辑屏；font_line 照旧生效（旧行为不变）");

    env_reset(); /* 128×32，s_fold_count=1 */

    /* ① font_line=0：区域 = 整块逻辑屏 */
    app_ldi_ctrl_vms_t *ctx = make_vms("AB_CD", 5, 1, 0, 0, 0);
    app_vms_ctrl(ctx, 5);

    /* FLAT 也包帧：begin → FILL → TEXT → end */
    check_one_frame(2);

    CHECK_MSG(s_rec_cnt == 2, "FLAT 应为 1 清屏 + 1 文本（2 次），得到 %d", s_rec_cnt);
    CHECK_MSG(count_type(APP_RENDER_TYPE_TEXT) == 1, "FLAT 只应渲染一次，得到 %d",
              count_type(APP_RENDER_TYPE_TEXT));
    const render_rec_t *r = nth_text(0);
    CHECK_MSG(r && r->x == 0 && r->y == 0 && r->w == 128 && r->h == 32,
              "FLAT 区域应为整块逻辑屏 128x32@(0,0)，得到 %ux%u@(%u,%u)", r ? (unsigned)r->w : 0U,
              r ? (unsigned)r->h : 0U, r ? (unsigned)r->x : 0U, r ? (unsigned)r->y : 0U);
    CHECK_MSG(r && r->len == 5 && r->text[0] == 'A' && r->text[2] == '\n',
              "FLAT 应把整段（含 '_'→'\\n'）一次交出");
    CHECK_MSG(r && r->has_style && !r->style.word_wrap && !r->style.prefer_one_line,
              "FLAT 样式不应开折叠开关（word_wrap/prefer_one_line 均为 false）");

    /* ② font_line=1：旧语义把内容放到第 1 条带（y=0, h=16），折叠开关不得介入 */
    env_reset();
    ctx = make_vms("AB", 2, 1, /*font_line=*/1, /*keep=*/0, /*font_size=*/0);
    app_vms_ctrl(ctx, 2);
    check_one_frame(2);
    const render_rec_t *r2 = nth_text(0);
    CHECK_MSG(r2 && r2->x == 0 && r2->y == 0 && r2->w == 128 && r2->h == 16,
              "FLAT font_line=1 应落在第 1 条带 128x16@(0,0)，得到 %ux%u@(%u,%u)",
              r2 ? (unsigned)r2->w : 0U, r2 ? (unsigned)r2->h : 0U, r2 ? (unsigned)r2->x : 0U,
              r2 ? (unsigned)r2->y : 0U);
}

/** 变体2 · E9 侧：只写上半屏、只清上半屏（下半留给 EA 预置图） */
static void case_variant2_e9_top_only(void)
{
    TEST_BEGIN("变体2：E9 只写上半、只清上半（下半留给 EA 预置图）");

    env_reset();
    s_fold_count = 2;
    declare_ea();
    s_scr_w = 224;
    s_scr_h = 100;

    app_ldi_ctrl_vms_t *ctx = make_vms("AB_CD", 5, 1, 0, 0, 0);
    app_vms_ctrl(ctx, 5);

    /* 1 次清屏（只清上半）+ 1 次上半文本；不得有下半文本 */
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "变体2 应恰好清屏一次，得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(count_type(APP_RENDER_TYPE_TEXT) == 1, "变体2 只应写上半 1 行，得到 %d",
              count_type(APP_RENDER_TYPE_TEXT));

    const render_rec_t *fill = nth_type(APP_RENDER_TYPE_FILL, 0);
    CHECK_MSG(fill && fill->x == 0 && fill->y == 0 && fill->w == 224 && fill->h == 50,
              "清屏应只覆盖上半 224x50@(0,0)，得到 %ux%u@(%u,%u)", fill ? (unsigned)fill->w : 0U,
              fill ? (unsigned)fill->h : 0U, fill ? (unsigned)fill->x : 0U, fill ? (unsigned)fill->y : 0U);

    const render_rec_t *top = nth_text(0);
    CHECK_MSG(top && top->x == 0 && top->y == 0 && top->w == 224 && top->h == 50,
              "文本应落上半 224x50@(0,0)");
    CHECK_MSG(top && top->len == 2 && top->text[0] == 'A' && top->text[1] == 'B',
              "上半文本应为 'AB'（'_' 转 '\\n' 后取第 1 段）");
    CHECK_MSG(top && top->has_style && top->style.word_wrap && top->style.prefer_one_line,
              "折叠样式 word_wrap + prefer_one_line 照旧");

    /* 变体2 E9：begin → 上半清屏 → 上半文字 → end */
    check_one_frame(2);

    /* E9 文本的颜色**不**影响 EA：EA 01H 用自身信号色（绿），与上面 font_color=2(红) 无关 */
    s_rec_cnt = 0;
    CHECK_MSG(app_fold_preset_show(1), "上半 E9 后 EA 应能取到绿槽");
    CHECK_MSG(nth_type(APP_RENDER_TYPE_BITMAP, 0) &&
                  nth_type(APP_RENDER_TYPE_BITMAP, 0)->color == DEV_DISPLAY_COLOR_GREEN,
              "EA 01H 应用自己的信号色绿，不跟上一条 E9 的红（解除耦合）");
}

/** 变体2 · EA 数据面：00H 只清下半（不调 BITMAP）；01H/02H 取对应槽；空槽拒画 */
static void case_ea_show_and_clear(void)
{
    TEST_BEGIN("EA：00H 只清下半（不调 BITMAP）；01H/02H 映射对应槽；空槽（黄）拒画");

    /* 00H → 只清下半屏：FILL(BLACK) 区域 = 下半，且不得有 BITMAP */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;

    CHECK_MSG(app_fold_lower_clear(), "清下半屏应返回 true");
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "应恰好一次 FILL，得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(count_type(APP_RENDER_TYPE_BITMAP) == 0, "清下半屏不得调 BITMAP");
    const render_rec_t *fill = nth_type(APP_RENDER_TYPE_FILL, 0);
    CHECK_MSG(fill && fill->x == 0 && fill->y == 50 && fill->w == 224 && fill->h == 50,
              "清屏区域应为下半 224x50@(0,50)");
    CHECK_MSG(fill && fill->color == DEV_DISPLAY_COLOR_BLACK, "清屏应为全黑");
    CHECK_MSG(fill && !fill->persist, "清下半屏不得落盘（清屏不落盘，上电有内容可判屏）");
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0,
              "清下半屏是单笔，不得 begin/end（包了无意义），得到 begin=%d end=%d", s_frame_begins,
              s_frame_ends);

    /* 01H → 绿槽：必须"先清下半屏再画"（bit=0 不动，不清会留残影），且两笔包成一帧 */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;
    CHECK_MSG(app_fold_preset_show(1), "绿预置图应显示成功");
    CHECK_MSG(s_rec_cnt == 2, "绿应为 清(1) + 画(1) 两次渲染，得到 %d", s_rec_cnt);
    CHECK_MSG(s_ev_cnt == 4 && s_ev[0] == EV_FRAME_BEGIN && s_ev[1] == EV_RENDER &&
                  s_ev[2] == EV_RENDER && s_ev[3] == EV_FRAME_END,
              "01H 事件序列应为 begin → FILL → BITMAP → end（begin/end 在两次渲染外侧）");
    CHECK_MSG(s_frame_begins == 1 && s_frame_ends == 1,
              "01H 应恰好 begin/end 各一次（成对），得到 begin=%d end=%d", s_frame_begins,
              s_frame_ends);
    CHECK_MSG(s_recs[0].type == APP_RENDER_TYPE_FILL && s_recs[1].type == APP_RENDER_TYPE_BITMAP,
              "顺序必须是 FILL → BITMAP（先清完再画）");
    CHECK_MSG(s_recs[0].x == 0 && s_recs[0].y == 50 && s_recs[0].w == 224 && s_recs[0].h == 50,
              "清屏区域应为下半 224x50@(0,50)");
    CHECK_MSG(s_recs[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏必须全黑");
    CHECK_MSG(!s_recs[0].persist, "预置图前的清屏不落盘（最终状态是图，由 BITMAP 落盘）");
    CHECK_MSG(s_recs[1].bitmap == s_bm_green, "01H 应取绿槽");
    CHECK_MSG(s_recs[1].x == 0 && s_recs[1].y == 50 && s_recs[1].w == 224 && s_recs[1].h == 50,
              "BITMAP 区域应为下半 224x50@(0,50)");
    CHECK_MSG(s_recs[1].color == DEV_DISPLAY_COLOR_GREEN,
              "01H 的颜色应取 Color 自己的信号色（绿），得到 %u", (unsigned)s_recs[1].color);
    CHECK_MSG(s_recs[1].persist, "EA 预置图应请求落盘（掉电恢复）");

    /* 02H → 红槽：同样"先清再画" */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;
    CHECK_MSG(app_fold_preset_show(2), "红预置图应显示成功");
    CHECK_MSG(s_rec_cnt == 2 && s_recs[0].type == APP_RENDER_TYPE_FILL &&
                  s_recs[1].type == APP_RENDER_TYPE_BITMAP,
              "红也应为 FILL → BITMAP（清完再画），得到 %d 次渲染", s_rec_cnt);
    CHECK_MSG(s_ev_cnt == 4 && s_ev[0] == EV_FRAME_BEGIN && s_ev[1] == EV_RENDER &&
                  s_ev[2] == EV_RENDER && s_ev[3] == EV_FRAME_END,
              "02H 也应为 begin → FILL → BITMAP → end（begin/end 在两次渲染外侧）");
    CHECK_MSG(s_frame_begins == 1 && s_frame_ends == 1, "02H 应恰好 begin/end 各一次（成对）");
    CHECK_MSG(s_recs[0].color == DEV_DISPLAY_COLOR_BLACK && !s_recs[0].persist,
              "红前的清屏应为全黑且不落盘（只 BITMAP 落盘）");
    CHECK_MSG(s_recs[1].bitmap == s_bm_red, "02H 应取红槽");
    CHECK_MSG(s_recs[1].color == DEV_DISPLAY_COLOR_RED,
              "02H 的颜色应取 Color 自己的信号色（红），得到 %u", (unsigned)s_recs[1].color);
    CHECK_MSG(s_recs[1].persist, "EA 预置图（红）也应请求落盘");

    /* 同一色连发两次：每次都要重新清 + 画，不得因"与上次同色"跳过（缓存会吃残影） */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;
    CHECK_MSG(app_fold_preset_show(1), "同色连发第一次（绿）应成功");
    CHECK_MSG(s_frame_begins == 1 && s_frame_ends == 1, "第一次也应各自 begin/end 一次");
    render_rec_t first_pair[2];
    memcpy(first_pair, s_recs, sizeof(first_pair));
    s_rec_cnt      = 0;
    s_ev_cnt       = 0;
    s_frame_begins = 0;
    s_frame_ends   = 0;
    CHECK_MSG(app_fold_preset_show(1), "同色连发第二次（仍为绿）也应成功");
    CHECK_MSG(s_rec_cnt == 2, "第二次同样要清 + 画（2 次），得到 %d", s_rec_cnt);
    CHECK_MSG(s_ev_cnt == 4 && s_ev[0] == EV_FRAME_BEGIN && s_ev[3] == EV_FRAME_END &&
                  s_frame_begins == 1 && s_frame_ends == 1,
              "同色重发第二次同样要有自己的 begin → 两笔 → end");
    CHECK_MSG(memcmp(first_pair, s_recs, sizeof(first_pair)) == 0,
              "同色重发的两次渲染序列必须逐字段一致（每次都重新清+画）");

    /* 03H → 黄槽是空槽：拒画 + false + 无渲染 + 不 begin（不留未配对 begin） */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;
    CHECK_MSG(!app_fold_preset_show(3), "空槽应拒画并返回 false");
    CHECK_MSG(s_rec_cnt == 0, "空槽不得发起任何渲染，得到 %d 次", s_rec_cnt);
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "空槽拒画不得 begin（否则脏标记被永久压住）");
}

/** EA 拒画：尺寸不符 / 非法颜色 / 非折叠 —— 全部 false 且不渲染 */
static void case_ea_reject(void)
{
    TEST_BEGIN("EA：尺寸不符 / 非法颜色 / 非折叠 → 拒画 + false");

    /* 尺寸不符：下半屏 224x48 ≠ 预置图 224x50 */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 96;
    CHECK_MSG(!app_fold_preset_show(1), "尺寸不符应拒画并返回 false");
    CHECK_MSG(s_rec_cnt == 0, "尺寸不符不得渲染");
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "尺寸不符不得 begin（拒画路径零 begin）");

    /* 非法颜色（>03H） */
    env_reset();
    s_fold_count = 2;
    s_scr_w      = 224;
    s_scr_h      = 100;
    CHECK_MSG(!app_fold_preset_show(0x04), "非法颜色应拒画并返回 false");
    CHECK_MSG(s_rec_cnt == 0, "非法颜色不得渲染");
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "非法颜色不得 begin（拒画路径零 begin）");

    /* 非折叠：没有下半屏 */
    env_reset();
    s_fold_count = 1;
    CHECK_MSG(!app_fold_lower_clear(), "非折叠时清下半屏应返回 false");
    CHECK_MSG(!app_fold_preset_show(1), "非折叠时预置图应返回 false");
    CHECK_MSG(s_rec_cnt == 0, "非折叠不得渲染");
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "非折叠清屏/预置图都不得 begin");
}

/** 单笔清屏路径（02H / 定时到点）**不包帧**：只有一笔填充，没有中间态可压 */
static void case_clean_and_timer_no_frame(void)
{
    TEST_BEGIN("02H 清屏 / 定时到点：单笔路径不包帧（零 begin/end）");

    /* 02H（非折叠）：单次 FILL，不 begin */
    env_reset();
    memset(s_vms_buf, 0, sizeof(s_vms_buf));
    app_ldi_ctrl_vms_t *cc = (app_ldi_ctrl_vms_t *)s_vms_buf;
    cc->device_func_type   = 0x02;
    cc->clear_type         = 1; /* 红 */
    app_vms_ctrl(cc, 0);
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "02H 应恰好一次 FILL，得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0,
              "02H 单笔清屏不得 begin/end，得到 begin=%d end=%d", s_frame_begins, s_frame_ends);

    /* 02H（变体2）：只清上半，仍不 begin */
    env_reset();
    s_fold_count = 2;
    declare_ea();
    s_scr_w = 224;
    s_scr_h = 100;
    memset(s_vms_buf, 0, sizeof(s_vms_buf));
    cc                   = (app_ldi_ctrl_vms_t *)s_vms_buf;
    cc->device_func_type = 0x02;
    cc->clear_type       = 1;
    app_vms_ctrl(cc, 0);
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "变体2 02H 应恰好一次 FILL（只清上半），得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "变体2 02H 单笔清屏不得 begin/end");

    /* 定时到点（变体1）：单次清屏，不 begin。白盒：本套件 TU-include 了 app_vms_ctrl.c，
       直接把定时器设成"已到点"，免得真等 keep_time 秒。 */
    env_reset();
    s_fold_count       = 2;
    s_scr_w            = 224;
    s_scr_h            = 100;
    s_vms_clear_tick   = 0; /* 0 ≤ 当前 tick → 立即到点 */
    s_vms_timer_active = true;
    app_vms_timer_poll();
    CHECK_MSG(count_type(APP_RENDER_TYPE_FILL) == 1, "到点应清屏一次，得到 %d",
              count_type(APP_RENDER_TYPE_FILL));
    CHECK_MSG(s_frame_begins == 0 && s_frame_ends == 0, "定时到点单笔清屏不得 begin/end");

    /* 定时器未激活：不渲染、不 begin */
    env_reset();
    s_vms_timer_active = false;
    app_vms_timer_poll();
    CHECK_MSG(s_rec_cnt == 0 && s_frame_begins == 0 && s_frame_ends == 0,
              "未激活定时器不得渲染/begin");
}

/** VMS 清屏颜色（02H clear_type）：86号文 1+7 色，未知值回落黑（清屏的安全默认） */
static void case_vms_clear_color(void)
{
    TEST_BEGIN("VMS 清屏：00H~07H = 黑/红/绿/黄/蓝/紫/青/白，08H 越界回落黑");

    /* 顺序与 dev_display_color_t 枚举一致；[8] 是越界回落 */
    static const dev_display_color_t want[] = {
        DEV_DISPLAY_COLOR_BLACK,  DEV_DISPLAY_COLOR_RED,   DEV_DISPLAY_COLOR_GREEN,
        DEV_DISPLAY_COLOR_YELLOW, DEV_DISPLAY_COLOR_BLUE,  DEV_DISPLAY_COLOR_PURPLE,
        DEV_DISPLAY_COLOR_CYAN,   DEV_DISPLAY_COLOR_WHITE, DEV_DISPLAY_COLOR_BLACK,
    };
    for (uint8_t code = 0U; code <= 8U; code++) {
        env_reset();
        memset(s_vms_buf, 0, sizeof(s_vms_buf));
        app_ldi_ctrl_vms_t *cc = (app_ldi_ctrl_vms_t *)s_vms_buf;
        cc->device_func_type   = 0x02;
        cc->clear_type         = code;
        app_vms_ctrl(cc, 0);

        const render_rec_t *fill = nth_type(APP_RENDER_TYPE_FILL, 0);
        CHECK_MSG(fill && fill->color == want[code], "clear_type=%02XH 应为颜色 %d，得到 %d", code,
                  (int)want[code], fill ? (int)fill->color : -1);
    }
}

/** VMS 字号（01H font_size）：按本板可用字号升序落档，超档取最大；00H=自适应 */
static void case_vms_font_size(void)
{
    TEST_BEGIN("VMS 字号：按本板升序落档（合成字库 16/32）、超档取最大、00H=自适应");

    /* 合成 g_board_font_lib.sizes = {16, 32}：0→自适应, 1→16, 2→32, 3+/08H→32（最大档） */
    static const struct {
        uint8_t         code;
        app_font_size_t want;
    } cases[] = {
        {0x00, APP_FONT_SIZE_SELF_ADAPT},
        {0x01, APP_FONT_SIZE_16},
        {0x02, APP_FONT_SIZE_32},
        {0x03, APP_FONT_SIZE_32}, /* 超档取最大 */
        {0x08, APP_FONT_SIZE_32},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        env_reset(); /* FLAT 128×32，font_line=0 → 单笔文本 */
        app_ldi_ctrl_vms_t *ctx = make_vms("AB", 2, /*format=*/1, /*font_line=*/0, /*keep=*/0,
                                           cases[i].code);
        app_vms_ctrl(ctx, 2);

        const render_rec_t *r = nth_text(0);
        CHECK_MSG(r && r->font_size == cases[i].want, "font_size=%02XH 应映射为 %d，得到 %d",
                  cases[i].code, (int)cases[i].want, r ? (int)r->font_size : -1);
    }
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m折叠屏：模式判定 / 变体1 / 变体2（E9 只写上半 + EA 预置图）\033[0m\n");

    case_mode();
    case_rect();
    case_split_lines();
    case_variant1_two_lines();
    case_variant1_one_line();
    case_variant1_three_lines();
    case_flat();
    case_variant2_e9_top_only();
    case_ea_show_and_clear();
    case_ea_reject();
    case_clean_and_timer_no_frame();
    case_vms_clear_color();
    case_vms_font_size();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
