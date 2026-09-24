/**
 * @file    test_render.c
 * @brief   文字渲染：区域契约（[x,x+w)×[y,y+h)、高度门禁、对齐防下溢）
 *
 * **为什么需要这个测试**：`_render_text` 是"测量趟 + 渲染趟"两阶段排版，区域越界
 * 与高度不足的失效**不报错、只画错**（内容跑到别的卡那半边、或半截字骑在卡缝上），
 * 上机靠人眼很难归因。本套件把排版的**几何**（每次 fill/bitmap 的 x/y/w/h）钉死。
 *
 * **打桩方式**：直接 TU-include `Application/Src/Render/app_render.c`（`s_line_widths`
 * / `s_render_font` / `_line_push` 等都是 file-static，从外部够不着，且要触达两趟的
 * 内部行为）。渲染目标换成 **capture 目标**：把每次 `fill`/`bitmap` 的 x/y/w/h 与颜色
 * 原样记下来，只断言坐标与尺寸，不比对像素内容（字库是假的）。
 *
 * 依赖打桩：
 *   · `g_board_font_lib` —— 合成字号/单元（16/32），只需"找得到三元组"；
 *   · `dev_storage_t`    —— `ops->read` 填确定性图案，字形内容不参与断言；
 *   · `app_cfg_sched_*`  —— `app_render.c` 的持久化路径引用到，给空桩；
 *   · `os_stub.c` / `text_cvt.c` —— 互斥量与 UTF8→GBK 转换。
 *
 * **分工**：capture-vs-画布的**逐像素对拍不在本套件做** —— test_screen_canvas.c 已有
 * 一份独立参考实现做对拍。本套件只钉 `app_render` 自身的排版几何（入口夹取、高度
 * 门禁、换行/截断、对齐偏移），两套互补、不重复堆桩。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dev_display.h"
#include "dev_storage.h"
#include "dev_w25qxx.h"
#include "dev_cfg_record.h"
#include "app_cfg_sched.h"
#include "app_render.h"

/* ================================================================
 *  合成字库（只需"能查到一个单元"；字形内容不参与断言）
 * ================================================================ */

static const app_font_unit_t s_units[] = {
    {.key = {.size = APP_FONT_SIZE_16, .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT},
     .unit_size = 256},
    {.key = {.size = APP_FONT_SIZE_16, .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT},
     .unit_size = 8192},
    {.key = {.size = APP_FONT_SIZE_32, .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT},
     .unit_size = 512},
    {.key = {.size = APP_FONT_SIZE_32, .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT},
     .unit_size = 16384},
};
static const app_font_size_t s_sizes[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_32};

const app_font_lib_desc_t g_board_font_lib = {
    .lib            = s_units,
    .lib_count      = sizeof(s_units) / sizeof(s_units[0]),
    .sizes          = s_sizes,
    .size_count     = sizeof(s_sizes) / sizeof(s_sizes[0]),
    .asc_index_base = 0x20,
    .gb_index       = APP_FONT_IDX_KIND_GB2312,
    .total_bytes    = 256 + 8192 + 512 + 16384,
};

/* ================================================================
 *  存储 / 显示 桩
 * ================================================================ */

static int32_t fake_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)d;
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(addr + i);
    return 0;
}
static const dev_storage_ops_t s_fake_ops = {.read = fake_read};
static dev_storage_t            s_font_dev = {.ops = &s_fake_ops, .capacity = 1U << 20};
static dev_display_t            s_dummy_dev;

dev_storage_t *dev_w25qxx_get(void) { return &s_font_dev; }
dev_display_t *dev_display_get(void) { return &s_dummy_dev; }

/* 直写实屏那条缝（`_rt()` 未设 target 时走）本用例全程设了 capture target，
 * 故这两个原语不会被调用 —— 只提供定义满足链接。 */
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      dev_display_color_t color)
{
    (void)dev;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}
void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, dev_display_color_t color)
{
    (void)dev;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)bitmap;
    (void)color;
}

/* ---- 配置调度器桩（app_render.c 的持久化路径引用到） ---- */
uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *desc)
{
    (void)desc;
    return 0xFF;
}
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *payload, uint16_t payload_len)
{
    (void)id;
    (void)payload;
    (void)payload_len;
    return 0;
}
dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *payload, uint16_t payload_cap,
                                          uint16_t *payload_len)
{
    (void)id;
    (void)payload;
    (void)payload_cap;
    (void)payload_len;
    return DEV_CFG_RECORD_STATE_EMPTY;
}
bool app_cfg_sched_ready(void) { return false; }
void app_cfg_sched_load_all(void) {}

/* ---- 被测：生产源码本体（static 内部结构只能靠 TU-include 触达） ---- */
#include "../Application/Src/Render/app_render.c"

/* ================================================================
 *  capture 渲染目标：只记几何
 * ================================================================ */

typedef struct {
    bool     is_bitmap;
    uint16_t x, y, w, h;
    uint8_t  color;
} draw_rec_t;

#define REC_MAX (800U)
static draw_rec_t s_recs[REC_MAX];
static int        s_rec_cnt;

static app_render_target_t s_cap;

static void capture_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         dev_display_color_t c)
{
    (void)ctx;
    if (s_rec_cnt < (int)REC_MAX) s_recs[s_rec_cnt++] = (draw_rec_t){false, x, y, w, h, (uint8_t)c};
}
static void capture_bitmap(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *bm, dev_display_color_t c)
{
    (void)ctx;
    (void)bm;
    if (s_rec_cnt < (int)REC_MAX) s_recs[s_rec_cnt++] = (draw_rec_t){true, x, y, w, h, (uint8_t)c};
}

/** @brief 设目标几何（rows=宽、cols=高，与 app_render_target_t 一致）并清记录 */
static void cap_reset(uint16_t w, uint16_t h)
{
    s_cap.rows = w;
    s_cap.cols = h;
    s_rec_cnt  = 0;
}

static int count_bitmaps(void)
{
    int n = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].is_bitmap) n++;
    return n;
}
static int count_bitmaps_at_y(uint16_t y)
{
    int n = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].is_bitmap && s_recs[i].y == y) n++;
    return n;
}

/* ================================================================
 *  带夹具的渲染包装
 * ================================================================ */

#define LINE_H  (16U) /* 16 号字行高 */
#define ASC_W   (8U)  /* 16 号 ASCII 半宽 */
#define GBK_W   (16U) /* 16 号汉字宽 */

/** @brief 走 APP_RENDER_TYPE_TEXT（16 号黑体、GBK 直通） */
static void render_text(const char *text, uint16_t len, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h, app_render_style_t *style)
{
    app_render(&(app_render_cfg_t){
        .type      = APP_RENDER_TYPE_TEXT,
        .x         = x,
        .y         = y,
        .w         = w,
        .h         = h,
        .color     = DEV_DISPLAY_COLOR_RED,
        .text      = text,
        .len       = len,
        .font_size = APP_FONT_SIZE_16,
        .font_type = APP_FONT_TYPE_HT,
        .text_enc  = APP_FONT_ENC_GBK,
        .style     = style,
    });
}

/** @brief 走 APP_RENDER_TYPE_TEXT + APP_FONT_ENC_UTF8（入口会先 UTF8→GBK 再排版） */
static void render_text_utf8(const char *text, uint16_t len, uint16_t x, uint16_t y, uint16_t w,
                             uint16_t h, app_render_style_t *style)
{
    app_render(&(app_render_cfg_t){
        .type      = APP_RENDER_TYPE_TEXT,
        .x         = x,
        .y         = y,
        .w         = w,
        .h         = h,
        .color     = DEV_DISPLAY_COLOR_RED,
        .text      = text,
        .len       = len,
        .font_size = APP_FONT_SIZE_16,
        .font_type = APP_FONT_TYPE_HT,
        .text_enc  = APP_FONT_ENC_UTF8,
        .style     = style,
    });
}

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

/* ================================================================
 *  用例：高度
 * ================================================================ */

/** 区域高 < 单行字高 → 一次绘制都没有（含显式 `\n` 的文本，二者同门禁） */
static void case_height_too_small(void)
{
    TEST_BEGIN("区域高 < 单行字高 → 一次绘制都没有（含 \\n 文本）");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};

    cap_reset(64, 64);
    render_text("AB", 2, 0, 0, 64, LINE_H - 1, &st);
    CHECK_MSG(s_rec_cnt == 0, "h=%u<行高 %u 却画了 %d 次", (unsigned)(LINE_H - 1),
              (unsigned)LINE_H, s_rec_cnt);

    cap_reset(64, 64);
    render_text("A\nB", 3, 0, 0, 64, LINE_H - 1, &st);
    CHECK_MSG(s_rec_cnt == 0, "含 \\n 且 h<行高，却画了 %d 次", s_rec_cnt);
}

/** 能容 k 行时恰好 k 行，第 k+1 行不得出现 */
static void case_exact_lines(void)
{
    TEST_BEGIN("能容 k 行时恰好 k 行，第 k+1 行不得出现");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};

    cap_reset(64, 64);
    /* 4 行文本，区域只放得下 2 行（h = 2×16） */
    render_text("A\nB\nC\nD", 7, 0, 0, 64, 2 * LINE_H, &st);
    CHECK_MSG(count_bitmaps() == 2, "应恰好画 2 行，得到 %d 个字形", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(0) == 1 && count_bitmaps_at_y(LINE_H) == 1,
              "两行应分别落在 y=0、y=%u", (unsigned)LINE_H);
    CHECK_MSG(count_bitmaps_at_y(2 * LINE_H) == 0, "第 3 行（y=%u）不该出现",
              (unsigned)(2 * LINE_H));
}

/** y>0 时高度判据必须用 y+h（不是把 h 当绝对下边界） */
static void case_y_offset_gate(void)
{
    TEST_BEGIN("y>0 时判据用 y+h（防把 h 当绝对下边界）");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true};

    cap_reset(64, 128);
    /* 区域 [20, 52) 放得下 2 行；宽 8 = 单个 ASCII 宽，迫使每字各占一行。
       旧语义把 h=32 当绝对下界 → 第二行 (36+16=52>32) 被丢弃。 */
    render_text("AB", 2, 0, 20, ASC_W, 2 * LINE_H, &st);
    CHECK_MSG(count_bitmaps() == 2, "区域 [20,52) 应放得下 2 行，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(20) == 1 && count_bitmaps_at_y(20 + LINE_H) == 1,
              "两行应分别落在 y=20、y=%u", (unsigned)(20 + LINE_H));
}

/* ================================================================
 *  用例：宽度
 * ================================================================ */

/** 超宽 wrap：续行回区域左 x */
static void case_width_wrap(void)
{
    TEST_BEGIN("超宽 wrap：续行回区域左 x");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true};

    cap_reset(64, 64);
    /* 区域 [10, 34) 宽 24 → 每行 3 个 ASCII；6 字 → 2 行 */
    render_text("ABCDEF", 6, 10, 0, 24, 64, &st);
    CHECK_MSG(count_bitmaps() == 6, "6 个字符应全部放下，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(0) == 3 && count_bitmaps_at_y(LINE_H) == 3,
              "每行应各 3 个字形");

    bool left_ok = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].is_bitmap && s_recs[i].y == LINE_H)
            if (s_recs[i].x != 10 && s_recs[i].x != 18 && s_recs[i].x != 26) left_ok = false;
    CHECK_MSG(left_ok, "续行起点不是区域左 x（应为 10/18/26）");
}

/** 超宽截断：逐字形整体截断，保留已放下的；不换行 */
static void case_width_truncate(void)
{
    TEST_BEGIN("超宽截断：整体截断、保留已放下的");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = false};

    cap_reset(64, 64);
    /* 区域宽 24 → 只放得下 3 个 ASCII；"ABCDEF" 的 D/E/F 整体丢弃 */
    render_text("ABCDEF", 6, 0, 0, 24, 64, &st);
    CHECK_MSG(count_bitmaps() == 3, "截断后应只画 3 个字形，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(LINE_H) == 0, "截断模式不该换行");

    bool xs_ok = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].is_bitmap && s_recs[i].x > 16) xs_ok = false;
    CHECK_MSG(xs_ok, "有字形落到区域右界之外");
}

/* ================================================================
 *  用例：对齐
 * ================================================================ */

/** 对齐在区域内算（x>0 + 居中/右） */
static void case_align_in_region(void)
{
    TEST_BEGIN("对齐在区域内算（x>0 + 居中/右）");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_CENTER};

    cap_reset(64, 64);
    render_text("A", 1, 10, 0, 16, 64, &st);
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].x == 14, "居中应为 10+(16-8)/2=14，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].x : 999U);

    st.h_align = APP_RENDER_ALIGN_RIGHT_DOWN;
    cap_reset(64, 64);
    render_text("A", 1, 10, 0, 16, 64, &st);
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].x == 18, "右对齐应为 10+(16-8)=18，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].x : 999U);
}

/** 区域窄于单字宽时不得产生巨大 cur_x（防无符号下溢） */
static void case_align_no_underflow(void)
{
    TEST_BEGIN("区域窄于单字宽：对齐偏移不产生巨大 cur_x（防下溢）");

    /* 区域宽 8 < 汉字宽 16。旧实现 (8-16) 在 uint16 下回绕 → 续行偏移约 65532 */
    const char *two_gbk = "\xD6\xD0\xCE\xC4"; /* "中文" */

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_CENTER, .word_wrap = true};
    cap_reset(64, 64);
    render_text(two_gbk, 4, 0, 0, 8, 64, &st);
    CHECK_MSG(count_bitmaps() == 2, "两个汉字应各占一行，得到 %d", count_bitmaps());
    bool sane = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].x > 64) sane = false;
    CHECK_MSG(sane, "居中出现了巨大 x（无符号下溢）；某次绘制 x>目标宽");

    st.h_align = APP_RENDER_ALIGN_RIGHT_DOWN;
    cap_reset(64, 64);
    render_text(two_gbk, 4, 0, 0, 8, 64, &st);
    sane = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].x > 64) sane = false;
    CHECK_MSG(sane, "右对齐出现了巨大 x（无符号下溢）");
}

/* ================================================================
 *  用例：越界
 * ================================================================ */

static void case_out_of_bounds(void)
{
    TEST_BEGIN("越界：x>=目标宽 / y>=目标高 → 不画");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};

    cap_reset(64, 64);
    render_text("A", 1, 64, 0, 8, 16, &st); /* x == 目标宽 */
    CHECK_MSG(s_rec_cnt == 0, "x==目标宽 仍画了 %d 次", s_rec_cnt);

    cap_reset(64, 64);
    render_text("A", 1, 0, 64, 8, 16, &st); /* y == 目标高 */
    CHECK_MSG(s_rec_cnt == 0, "y==目标高 仍画了 %d 次", s_rec_cnt);
}

/** 区域部分出目标：夹取后不得越界，续行回夹取后的区域左 */
static void case_clamp_partial(void)
{
    TEST_BEGIN("区域部分出目标：夹取后坐标/尺寸不越界");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true};

    cap_reset(64, 64);
    /* 请求 [10, 1010) × [0, 1010) → 夹到 [10,64) × [0,64) */
    const char *t = "AAAAAAAAAAAAAAAAAAAA"; /* 20 个 ASCII */
    render_text(t, 20, 10, 0, 1000, 1000, &st);

    CHECK_MSG(count_bitmaps() == 20, "全部 20 个字符都应放下，得到 %d", count_bitmaps());

    bool in_bounds = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if ((uint32_t)s_recs[i].x + s_recs[i].w > 64 || (uint32_t)s_recs[i].y + s_recs[i].h > 64)
            in_bounds = false;
    CHECK_MSG(in_bounds, "有绘制超出目标几何 64×64（入口未夹取）");

    /* 区域宽夹到 54：每行 6 个 ASCII（x=10..50） */
    CHECK_MSG(count_bitmaps_at_y(LINE_H) == 6, "第二行应有 6 个字形，得到 %d",
              count_bitmaps_at_y(LINE_H));
    bool left_ok = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].is_bitmap && s_recs[i].y == LINE_H && s_recs[i].x % 8 != 2) left_ok = false;
    CHECK_MSG(left_ok, "续行起点不是夹取后的区域左 x=10（及 10+8k）");
}

/* ================================================================
 *  用例：行数（A2 回归）
 * ================================================================ */

/** ≥33 行文本不得破坏栈（原 `uint16_t line_widths[32]` 越界写） */
static void case_many_lines(void)
{
    TEST_BEGIN("≥33 行文本不再破坏栈（A2 回归）");

    static char t[96];
    uint16_t    n = 0;
    for (int i = 0; i < 33; i++) {
        t[n++] = 'A';
        if (i < 32) t[n++] = '\n';
    }

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};
    cap_reset(64, 33 * LINE_H);
    render_text(t, n, 0, 0, 64, 33 * LINE_H, &st);

    CHECK_MSG(count_bitmaps() == 33, "33 行应全部画出，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y((uint16_t)(32 * LINE_H)) == 1, "第 33 行应在 y=%u",
              (unsigned)(32 * LINE_H));
}

/* ================================================================
 *  用例：cvt 边界安全（P4）
 * ================================================================ */

/** 2 字节 + 3 字节 + 4 字节混合：前后文字必须仍在、emoji 被安全跳过 */
static void case_cvt_mixed(void)
{
    TEST_BEGIN("cvt：2/3/4 字节混合——前后文字仍在、emoji 安全跳过");

    char     out[64];
    uint32_t n = 0;

    /* é(2B, U+00E9→A8A6) 中(3B, D6D0) 😀(4B, 跳过) 文(3B, CEC4) */
    const char *mixed = "é中😀文";
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_gbk(mixed, (uint32_t)strlen(mixed), out, &n);

    const uint8_t exp[] = {0xA8, 0xA6, 0xD6, 0xD0, 0xCE, 0xC4};
    bool          same  = (n == sizeof(exp));
    for (size_t i = 0; i < sizeof(exp) && same; i++)
        if ((uint8_t)out[i] != exp[i]) same = false;
    CHECK_MSG(same, "é中😀文 应为 A8A6 D6D0 CEC4（6 字节），得到 out=%u", (unsigned)n);

    /* emoji 在开头：它后面的文字一个字都不能丢 */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_gbk("😀中文", (uint32_t)strlen("😀中文"), out, &n);
    CHECK_MSG(n == 4 && (uint8_t)out[0] == 0xD6 && (uint8_t)out[1] == 0xD0 &&
                  (uint8_t)out[2] == 0xCE && (uint8_t)out[3] == 0xC4,
              "😀中文 应为 D6D0 CEC4，得到 out=%u", (unsigned)n);
}

/** 输入长度切在多字节中间：干净停止、不越界（本套件跑在 ASan 下） */
static void case_cvt_truncated(void)
{
    TEST_BEGIN("cvt：长度切在多字节中间——不越界、只返回已转换部分");

    char     out[64];
    uint32_t n = 0;

    /* "中重" 只喂 5 字节：中(3) 完整，重(3) 只到 2 → 只输出 D6D0 */
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_gbk("中重", 5, out, &n);
    CHECK_MSG(n == 2 && (uint8_t)out[0] == 0xD6 && (uint8_t)out[1] == 0xD0,
              "截在第二个汉字中间应只输出 D6D0，得到 out=%u", (unsigned)n);

    /* "重" 只喂 2 字节：整字不足 → 一个字节都不输出 */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_gbk("重", 2, out, &n);
    CHECK_MSG(n == 0, "不足一个完整 3 字节序列应输出 0，得到 out=%u", (unsigned)n);

    /* 同类下溢：GBK→UTF8 喂半个 GBK 码；UTF8→UNICODE 喂半个序列 */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_gbk_to_utf8("\xD6", 1, out, &n);
    CHECK_MSG(n == 0, "半个 GBK 码应干净停止，得到 out=%u", (unsigned)n);

    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_unicode("\xE4\xB8", 2, out, &n);
    CHECK_MSG(n == 0, "不足一个 UTF-8 序列应干净停止，得到 out=%u", (unsigned)n);

    /* 2 字节序列正确解码（é = U+00E9，小端 E9 00） */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_utf8_to_unicode("é", 2, out, &n);
    CHECK_MSG(n == 2 && (uint8_t)out[0] == 0xE9 && (uint8_t)out[1] == 0x00,
              "é 应解出 U+00E9（小端 E9 00），得到 out=%u", (unsigned)n);
}

/** 长 UTF-8 输入经 app_render：输入夹到 text_buf，输出不越界且排版合理 */
static void case_long_utf8_clamp(void)
{
    TEST_BEGIN("长 UTF-8 输入经 app_render：输入夹到 text_buf，不越界且排版合理");

    static char src[3 * 110];
    for (int i = 0; i < 110; i++) memcpy(src + 3 * i, "中", 3);
    const uint32_t len = 3 * 110; /* 330B > sizeof(text_buf)=256 */

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};

    /* 目标足够宽：256B 输入里含 85 个完整 3 字节汉字（第 256 字节是半截，丢弃） */
    cap_reset(1400, 64);
    render_text_utf8(src, (uint16_t)len, 0, 0, 1400, 64, &st);
    CHECK_MSG(count_bitmaps() == 85, "应画 85 个汉字（256B/3 向下取整），得到 %d",
              count_bitmaps());
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m文字渲染区域契约（字号 16，ASCII 半宽 8 / 汉字宽 16）\033[0m\n");

    /* 夹具：目标几何由 cap_reset 每次设置；s_render_display 只为通过 app_render 的门卫 */
    s_dummy_dev.screen_rows = 64;
    s_dummy_dev.screen_cols = 64;
    s_render_display        = &s_dummy_dev;
    s_render_font           = &s_font_dev;

    s_cap.fill   = capture_fill;
    s_cap.bitmap = capture_bitmap;
    s_cap.ctx    = nullptr;
    s_cap.rows   = 64;
    s_cap.cols   = 64;
    app_render_set_target(&s_cap);

    case_height_too_small();
    case_exact_lines();
    case_y_offset_gate();
    case_width_wrap();
    case_width_truncate();
    case_align_in_region();
    case_align_no_underflow();
    case_out_of_bounds();
    case_clamp_partial();
    case_many_lines();
    case_cvt_mixed();
    case_cvt_truncated();
    case_long_utf8_clamp();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
