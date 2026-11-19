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
 *
 * **还覆盖**（同套件顺带，不另立文件）：
 *   · `app_render()` 的模块互斥量 —— 每次调用（含提前返回）恰好一次 Acquire/Release；
 *   · `text_cvt.c` 的 UTF8/GBK/UNICODE 互转边界（截在多字节序列中间、半个码位）；
 *   · RLS 命令层（`app_rls_cmd.c`）：显示帧的位图长度门禁（不足**拒显且不碰
 *     app_render**，下界钉 `data_len ≤ 13` 的守卫防无符号减法回绕）、颜色映射与表外
 *     拒显、亮度 0~31/255 的档位换算、修改 IP 的落盘与端口保留、搜索应答帧的逐字节
 *     布局与 BCC。
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
#include "app_iap_cfg.h"
#include "app_render.h"

/* os_stub.c 的互斥获取/释放计数（见 os_stub.c 的互斥段）；
 * 用于断言 app_render() 每次调用恰好一次 Acquire/Release。 */
extern uint32_t g_os_mutex_acquire_count;
extern uint32_t g_os_mutex_release_count;

/* ================================================================
 *  合成字库（只需"能查到一个单元"；字形内容不参与断言）
 * ================================================================ */

/* 字号集合取与 3833024 一致的一组 {14,16,20,24,32}（**必须升序**）：自适应"按实际行
   选字号"的现场案例（128×32、9 字节两行）只有在含 24 的集合里才会暴露旧 blob 估算的
   缺陷，故这里不能只留 16/32。每个字号都给 ASCII/GBK 两个单元，`_find_unit` 才找得到。 */
#define SZ(n) APP_FONT_SIZE_##n
static const app_font_unit_t s_units[] = {
    {.key = {.size = SZ(14), .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT}, .unit_size = 256},
    {.key = {.size = SZ(14), .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT}, .unit_size = 8192},
    {.key = {.size = SZ(16), .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT}, .unit_size = 256},
    {.key = {.size = SZ(16), .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT}, .unit_size = 8192},
    {.key = {.size = SZ(20), .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT}, .unit_size = 320},
    {.key = {.size = SZ(20), .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT}, .unit_size = 10240},
    {.key = {.size = SZ(24), .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT}, .unit_size = 384},
    {.key = {.size = SZ(24), .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT}, .unit_size = 12288},
    {.key = {.size = SZ(32), .charset = APP_FONT_ENC_ASCII, .type = APP_FONT_TYPE_HT}, .unit_size = 512},
    {.key = {.size = SZ(32), .charset = APP_FONT_ENC_GBK, .type = APP_FONT_TYPE_HT}, .unit_size = 16384},
};
static const app_font_size_t s_sizes[] = {SZ(14), SZ(16), SZ(20), SZ(24), SZ(32)};

const app_font_lib_desc_t g_board_font_lib = {
    .lib            = s_units,
    .lib_count      = sizeof(s_units) / sizeof(s_units[0]),
    .sizes          = s_sizes,
    .size_count     = sizeof(s_sizes) / sizeof(s_sizes[0]),
    .asc_index_base = 0x20,
    .gb_index       = APP_FONT_IDX_KIND_GB2312,
    .total_bytes    = 2 * 256 + 2 * 8192 + 320 + 10240 + 384 + 12288 + 512 + 16384,
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

/* ---- app_screen 逻辑屏几何桩 ----
 * app_rls_cmd.c 的显示 handler 经 app_screen_rows/cols() 读**逻辑屏**几何来算所需
 * 位图字节。这里给一组可调桩，用例按板级几何（128×32 / 224×100）设定。 */
static uint16_t s_stub_rows = 64;
static uint16_t s_stub_cols = 64;
uint16_t        app_screen_rows(void) { return s_stub_rows; }
uint16_t        app_screen_cols(void) { return s_stub_cols; }

/* ---- 屏开关桩：DISPLAY_SW 只切面板输出使能，不碰亮度 ---- */
static int s_oe_enable_cnt;
static int s_oe_disable_cnt;

void app_screen_oe_enable(void) { s_oe_enable_cnt++; }
void app_screen_oe_disable(void) { s_oe_disable_cnt++; }

/* ---- app_light_sensor 桩：显示命令会把帧内亮度应用到整屏 ----
 * 真实实现要停光感跟随、再走 app_screen_set_brightness（级联下发），host 上不跑；
 * 这里只记"调了哪个 API、参数是什么"。 */
static int     s_light_fixed_cnt;
static uint8_t s_light_fixed_last;
static int     s_light_resume_cnt;

void app_light_sensor_set_fixed(uint8_t level)
{
    s_light_fixed_cnt++;
    s_light_fixed_last = level;
}
void app_light_sensor_resume(void) { s_light_resume_cnt++; }

/* ---- 被测：生产源码本体（static 内部结构只能靠 TU-include 触达） ---- */
#include "../Application/Src/Render/app_render.c"

/* ---- 被测：RLS 命令处理（显示帧位图长度校验） ----
 * 这里 TU-include 而不是在 test_probes.c 测：本套件已有**真实** app_render 与
 * capture 目标，能直接断言"位图不足时一次 app_render 都不许调"；放 probes 那侧
 * 还得再造一份 app_render/app_screen 桩与调用计数，两处更容易漂移。 */
#include "../Application/Src/RLS/app_rls_cmd.c"

/* ---- 网络 / 持久化 / 通道桩：RLS 的修改 IP 与搜索应答 ----
 * 真实现要走内部 Flash 记录与 LwIP，host 上不跑；这里只记"传了什么、发了什么"。 */
static int     s_tx_cnt;
static int     s_tx_len;
static uint8_t s_tx_buf[64];

int32_t app_ccb_send(app_ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    s_tx_cnt++;
    s_tx_len = (len <= sizeof(s_tx_buf)) ? (int)len : (int)sizeof(s_tx_buf);
    if (s_tx_len > 0) memcpy(s_tx_buf, data, (size_t)s_tx_len);
    return (int32_t)len;
}

static uint8_t s_net_ip[4]   = {192, 168, 1, 254};
static uint8_t s_net_mask[4] = {255, 255, 255, 0};
static uint8_t s_net_gw[4]   = {192, 168, 1, 1};

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_net_ip, 4);
    memcpy(mask, s_net_mask, 4);
    memcpy(gw, s_net_gw, 4);
}

static app_flash_iap_net_cfg_t s_iap_rec;
static bool                    s_iap_rec_valid;
static uint8_t                 s_iap_new_ip[4];
static uint8_t                 s_iap_new_mask[4];
static uint8_t                 s_iap_new_gw[4];
static uint32_t                s_iap_new_port;
static int                     s_iap_upd_cnt;
static uint16_t                s_tcp_port = 10028;

bool app_iap_get_net_cfg(app_flash_iap_net_cfg_t *out)
{
    if (!s_iap_rec_valid) return false;

    *out = s_iap_rec;
    return true;
}

void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    s_iap_upd_cnt++;
    memcpy(s_iap_new_ip, ip, 4);
    memcpy(s_iap_new_mask, mask, 4);
    memcpy(s_iap_new_gw, gw, 4);
    s_iap_new_port = port;
}

uint16_t app_tcp_server_get_port(void) { return s_tcp_port; }

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

/** 绘制序列快照：用于"开关关掉 = 旧行为"的逐条对拍（顺序、坐标、尺寸、颜色全比） */
static draw_rec_t s_snap[REC_MAX];
static int        s_snap_cnt;

static void snapshot_take(void)
{
    s_snap_cnt = s_rec_cnt;
    if (s_rec_cnt > 0) memcpy(s_snap, s_recs, (size_t)s_rec_cnt * sizeof(draw_rec_t));
}
static bool snapshot_equal(void)
{
    if (s_snap_cnt != s_rec_cnt) return false;
    return memcmp(s_snap, s_recs, (size_t)s_rec_cnt * sizeof(draw_rec_t)) == 0;
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

/** @brief 走 APP_RENDER_TYPE_TEXT + **自适应字号**（16/32 由其按区域与样式选）——
 *         prefer_one_line 的用例必须走这条，否则字号被固定成 16 看不到选择效果。 */
static void render_text_auto(const char *text, uint16_t len, uint16_t x, uint16_t y, uint16_t w,
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
        .font_size = APP_FONT_SIZE_SELF_ADAPT,
        .font_type = APP_FONT_TYPE_HT,
        .text_enc  = APP_FONT_ENC_GBK,
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
 *  用例：模块互斥量（P7）
 * ================================================================ */

/** @brief app_render 的模块锁：每次调用（含内部提前返回）恰好一次 acquire/release */
static void case_render_lock_balance(void)
{
    TEST_BEGIN("app_render 每次调用恰好一次 acquire/release（含提前返回路径）");

    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP};

    const uint32_t a0 = g_os_mutex_acquire_count;
    const uint32_t r0 = g_os_mutex_release_count;
    int            calls = 0;

    /* 正常渲染（走完整个内部流程） */
    cap_reset(64, 64);
    render_text("AB", 2, 0, 0, 64, LINE_H, &st);
    calls++;

    /* 提前返回：区域高 < 行高，_render_text 内部 return */
    cap_reset(64, 64);
    render_text("AB", 2, 0, 0, 64, LINE_H - 1, &st);
    calls++;

    /* 提前返回：type 越界，跳表取不到函数（仍要成对加解锁） */
    app_render(&(app_render_cfg_t){.type = (app_render_type_t)7});
    calls++;

    const uint32_t da = g_os_mutex_acquire_count - a0;
    const uint32_t dr = g_os_mutex_release_count - r0;
    CHECK_MSG(da == (uint32_t)calls && dr == (uint32_t)calls,
              "%d 次调用应各一次 acquire/release，得到 acquire=%u release=%u", calls,
              (unsigned)da, (unsigned)dr);
}

/* ================================================================
 *  用例：高度
 * ================================================================ */

/** @brief 区域高 < 单行字高 → 一次绘制都没有（含显式 `\n` 的文本，二者同门禁） */
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

/** @brief 能容 k 行时恰好 k 行，第 k+1 行不得出现 */
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

/** @brief y>0 时高度判据必须用 y+h（不是把 h 当绝对下边界） */
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

/** @brief 超宽 wrap：续行回区域左 x */
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

/** @brief 超宽截断：逐字形整体截断，保留已放下的；不换行 */
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

/** @brief 对齐在区域内算（x>0 + 居中/右） */
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

/** @brief 区域窄于单字宽时不得产生巨大 cur_x（防无符号下溢） */
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

/** @brief 越界：x/y 到达或越过目标尺寸时一次都不画
 *
 *  判据必须含**等于**（`x >= 目标宽` / `y >= 目标高`）—— 差一个比较符就会在屏的
 *  右/下边缘画出半张字形，而裁剪会把它悄悄吃掉，现场只表现为"贴边那个字少一块"。 */
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

/** @brief 区域部分出目标：夹取后不得越界，续行回夹取后的区域左 */
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

/** @brief ≥33 行文本不得破坏栈（原 `uint16_t line_widths[32]` 越界写） */
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
 *  用例：prefer_one_line（S2）
 * ================================================================ */

/** @brief 自适应优先单行：单行放得下就选能放下的**最大**字号且不换行；
 *  连最小字号一行都放不下才退回最小字号并强制换行。
 *
 *  合成字库 sizes={14,16,20,24,32}（与 3833024 一致）。GBK 字形宽 = size，ASCII = size/2。 */
static void case_prefer_one_line(void)
{
    TEST_BEGIN("prefer_one_line：单行放得下→大字号且不换行；最小字号都放不下→换行");

    /* ① 单行放得下：区域 64x32，"中中"(2 汉字) 在 32 号下宽 2×32=64 ≤ 64 且字号 32 ≤ 32
          → 选 32 号（比最小 16 号大），且只有一行 */
    const char *two_gbk = "\xD6\xD0\xD6\xD0"; /* "中中" */
    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .prefer_one_line = true};
    cap_reset(64, 32);
    render_text_auto(two_gbk, 4, 0, 0, 64, 32, &st);
    CHECK_MSG(count_bitmaps() == 2, "两个汉字应各画一次，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(0) == 2, "两个汉字都应落在 y=0（单行），得到 %d",
              count_bitmaps_at_y(0));
    CHECK_MSG(count_bitmaps_at_y(32) == 0, "不该出现第二行（y=32）");
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 32 && s_recs[0].h == 32,
              "应选到更大字号 32（字形 32x32），得到 %ux%u", s_rec_cnt ? (unsigned)s_recs[0].w : 0U,
              s_rec_cnt ? (unsigned)s_recs[0].h : 0U);

    /* ② 连最小字号一行都放不下：区域 48x64，"中中中中"(4 汉字) 在最小字号 14 下宽 56 > 48
          → 取最小字号 14 且强制换行（3 个在 y=0、1 个在 y=14），不越区域 */
    const char *four_gbk = "\xD6\xD0\xD6\xD0\xD6\xD0\xD6\xD0"; /* "中中中中" */
    app_render_style_t st2 = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .prefer_one_line = true};
    cap_reset(48, 64);
    render_text_auto(four_gbk, 8, 0, 0, 48, 64, &st2);
    CHECK_MSG(count_bitmaps() == 4, "4 个汉字应全部放下，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(0) == 3 && count_bitmaps_at_y(14) == 1,
              "应换行：y=0 放 3 个、y=14 放 1 个，得到 %d / %d", count_bitmaps_at_y(0),
              count_bitmaps_at_y(14));
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 14, "应退回最小字号 14，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].w : 0U);
    bool in_bounds = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if ((uint32_t)s_recs[i].x + s_recs[i].w > 48 || (uint32_t)s_recs[i].y + s_recs[i].h > 64)
            in_bounds = false;
    CHECK_MSG(in_bounds, "换行后不得越出区域 48x64");

    /* ③ 开关是真在起作用：同一输入(64x64、word_wrap)下，prefer 选**放得下单行**的字号
          且不换行；关掉开关走旧的按面积自适应会选更大字号并换行。 */
    const char *three_gbk = "\xD6\xD0\xD6\xD0\xD6\xD0"; /* "中中中" */
    app_render_style_t on  = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true,
                              .prefer_one_line = true};
    app_render_style_t off = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true};

    cap_reset(64, 64);
    render_text_auto(three_gbk, 6, 0, 0, 64, 64, &on);
    CHECK_MSG(count_bitmaps() == 3 && count_bitmaps_at_y(32) == 0, "打开开关应单行不换行（第二行 y=32 不出现）");
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 20, "打开开关应选放得下单行的最大字号 20（3×20=60≤64），得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].w : 0U);

    cap_reset(64, 64);
    render_text_auto(three_gbk, 6, 0, 0, 64, 64, &off);
    CHECK_MSG(count_bitmaps_at_y(32) == 1, "关掉开关走旧自适应会换行（32 号一行放不下，第二行 y=32）");
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 32, "关掉开关旧逻辑选 32 号，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].w : 0U);

    /* ④ prefer_one_line=false → 与不含该字段的旧构造**逐条相同**（同一输入对拍） */
    app_render_style_t a = {.h_align = APP_RENDER_ALIGN_CENTER, .word_wrap = true,
                            .prefer_one_line = false};
    app_render_style_t b = {.h_align = APP_RENDER_ALIGN_CENTER, .word_wrap = true};
    cap_reset(64, 64);
    render_text_auto(three_gbk, 6, 0, 0, 64, 64, &a);
    snapshot_take();
    cap_reset(64, 64);
    render_text_auto(three_gbk, 6, 0, 0, 64, 64, &b);
    CHECK_MSG(snapshot_equal(), "prefer_one_line=false 的绘制序列必须与旧构造逐条相同");

    /* ⑤ 文本含显式 `\\n` → 开关不生效（逐字走旧行为） */
    const char *with_nl = "A\nB";
    app_render_style_t nl_on  = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .prefer_one_line = true};
    app_render_style_t nl_off = {.h_align = APP_RENDER_ALIGN_LEFT_UP};
    cap_reset(64, 64);
    render_text_auto(with_nl, 3, 0, 0, 64, 64, &nl_on);
    snapshot_take();
    cap_reset(64, 64);
    render_text_auto(with_nl, 3, 0, 0, 64, 64, &nl_off);
    CHECK_MSG(snapshot_equal(), "文本含 \\n 时 prefer_one_line 不该生效（应与旧行为一致）");
}

/* ================================================================
 *  用例：自适应字号 · 含 `\n`（现场回归 R）
 * ================================================================ */

/** @brief 含 `\n` 的多行文本：旧 blob 估算看不到行数 —— 128×32、9 字节"回归\n测试"在 24 号
 *  命中（h_res=1、w_res=10 → 10≥9），但 24 号两行=48>32 → 只画第一行、居中也失效。
 *  新口径按"行数 × 字号 ≤ rh 且最宽行 ≤ rw"选，应选 16。 */
static void case_adaptive_multiline(void)
{
    TEST_BEGIN("自适应·含\\n：128x32 的\"回归\\n测试\"应选 16 号、两行各 16（总高 32）");

    const char *two_line = "\xBB\xD8\xB9\xE9\n\xB2\xE2\xCA\xD4"; /* "回归\n测试" = 9B */
    app_render_style_t st = {.h_align = APP_RENDER_ALIGN_LEFT_UP,
                             .v_align = APP_RENDER_ALIGN_CENTER}; /* 与 LDI FLAT 同款：不换行 */

    cap_reset(128, 32);
    render_text_auto(two_line, 9, 0, 0, 128, 32, &st);

    CHECK_MSG(count_bitmaps() == 4, "4 个汉字应各画一次，得到 %d", count_bitmaps());
    CHECK_MSG(count_bitmaps_at_y(0) == 2 && count_bitmaps_at_y(16) == 2,
              "应两行各 2 个：y=0 / y=16，得到 %d / %d", count_bitmaps_at_y(0),
              count_bitmaps_at_y(16));
    bool     all16      = (s_rec_cnt > 0);
    uint16_t max_bottom = 0;
    for (int i = 0; i < s_rec_cnt; i++) {
        if (s_recs[i].w != 16 || s_recs[i].h != 16) all16 = false;
        if ((uint32_t)s_recs[i].y + s_recs[i].h > max_bottom)
            max_bottom = (uint16_t)(s_recs[i].y + s_recs[i].h);
    }
    CHECK_MSG(all16, "应选 16 号（字形 16x16），而不是 24 号");
    CHECK_MSG(max_bottom == 32, "两行总高应 = 32（占满区域，居中成立），得到 %u", (unsigned)max_bottom);

    /* 极端：3 行在 32 高里放不下 → 取最小字号 14，只画放得下的行、不越区域、不画半截 */
    const char *three_line = "\xD2\xBB\n\xB6\xFE\n\xC8\xFD"; /* "一\n二\n三" = 8B */
    app_render_style_t st2 = {.h_align = APP_RENDER_ALIGN_LEFT_UP};
    cap_reset(128, 32);
    render_text_auto(three_line, 8, 0, 0, 128, 32, &st2);
    bool in_bounds = true;
    for (int i = 0; i < s_rec_cnt; i++)
        if ((uint32_t)s_recs[i].x + s_recs[i].w > 128 || (uint32_t)s_recs[i].y + s_recs[i].h > 32)
            in_bounds = false;
    CHECK_MSG(in_bounds, "3 行放不下时不得越出区域 128x32");
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 14, "应取最小字号 14，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].w : 0U);
    CHECK_MSG(count_bitmaps() == 2, "32 高只放得下 2 行（14×2=28），第 3 行不画，得到 %d",
              count_bitmaps());

    /* 无 `\n` 的单行：仍是旧 blob 口径（64x64、"中中中" → 32 号并换行） */
    const char *one_line = "\xD6\xD0\xD6\xD0\xD6\xD0"; /* "中中中" */
    app_render_style_t st3 = {.h_align = APP_RENDER_ALIGN_LEFT_UP, .word_wrap = true};
    cap_reset(64, 64);
    render_text_auto(one_line, 6, 0, 0, 64, 64, &st3);
    CHECK_MSG(s_rec_cnt >= 1 && s_recs[0].w == 32, "无 \\n 单行仍按旧 blob 口径选 32，得到 %u",
              s_rec_cnt ? (unsigned)s_recs[0].w : 0U);
    CHECK_MSG(count_bitmaps_at_y(32) == 1, "无 \\n 单行仍是旧换行结果（第二行 y=32 放 1 个）");
}

/* ================================================================
 *  用例：cvt 边界安全（P4）
 * ================================================================ */

/** @brief 2 字节 + 3 字节 + 4 字节混合：前后文字必须仍在、emoji 被安全跳过 */
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

/** @brief 输入长度切在多字节中间：干净停止、不越界（本套件跑在 ASan 下） */
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

/** @brief GBK↔UNICODE 与 UNICODE→UTF8（P4 同批残留）：
 *  半个码位干净停止、字节序/输出语义保持不变 */
static void case_cvt_unicode_bounds(void)
{
    TEST_BEGIN("cvt：GBK/UNICODE 互转与 UNICODE→UTF8 —— 半个码位干净停止、语义不变");

    char     out[64];
    uint32_t n = 0;

    /* GBK "A中"(41 D6D0) → UNICODE 小端：ASCII 也占 2B（41 00），中=U+4E2D → 2D 4E */
    memset(out, 0xEE, sizeof(out));
    cvt_gbk_to_unicode("\x41\xD6\xD0", 3, out, &n);
    const uint8_t exp_g2u[] = {0x41, 0x00, 0x2D, 0x4E};
    bool          ok        = (n == sizeof(exp_g2u));
    for (size_t i = 0; i < sizeof(exp_g2u) && ok; i++)
        if ((uint8_t)out[i] != exp_g2u[i]) ok = false;
    CHECK_MSG(ok, "A中 → 41 00 2D 4E，得到 %u 字节", (unsigned)n);

    /* 半个 GBK 码：一个字节都不输出（此前 fromSize -= 2 会无符号下溢） */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_gbk_to_unicode("\xD6", 1, out, &n);
    CHECK_MSG(n == 0, "半个 GBK 码应干净停止，得到 %u", (unsigned)n);

    /* UNICODE 小端 "中A"(2D 4E 41 00) → GBK 大端 D6D0 + 'A'(1 字节) */
    memset(out, 0xEE, sizeof(out));
    cvt_unicode_to_gbk("\x2D\x4E\x41\x00", 4, out, &n);
    const uint8_t exp_u2g[] = {0xD6, 0xD0, 0x41};
    ok                      = (n == sizeof(exp_u2g));
    for (size_t i = 0; i < sizeof(exp_u2g) && ok; i++)
        if ((uint8_t)out[i] != exp_u2g[i]) ok = false;
    CHECK_MSG(ok, "中A → D6 D0 41，得到 %u 字节", (unsigned)n);

    /* 半个 UTF-16 码元：干净停止 */
    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_unicode_to_gbk("\x2D", 1, out, &n);
    CHECK_MSG(n == 0, "半个 UNICODE 码元应干净停止，得到 %u", (unsigned)n);

    /* UNICODE 小端 "中é"(2D 4E E9 00) → UTF-8：E4B8AD + C3A9 */
    memset(out, 0xEE, sizeof(out));
    cvt_unicode_to_utf8("\x2D\x4E\xE9\x00", 4, out, &n);
    const uint8_t exp_u2u8[] = {0xE4, 0xB8, 0xAD, 0xC3, 0xA9};
    ok                       = (n == sizeof(exp_u2u8));
    for (size_t i = 0; i < sizeof(exp_u2u8) && ok; i++)
        if ((uint8_t)out[i] != exp_u2u8[i]) ok = false;
    CHECK_MSG(ok, "中é → E4 B8 AD C3 A9，得到 %u 字节", (unsigned)n);

    n = 0;
    memset(out, 0xEE, sizeof(out));
    cvt_unicode_to_utf8("\x2D", 1, out, &n);
    CHECK_MSG(n == 0, "半个 UNICODE 码元转 UTF-8 应干净停止，得到 %u", (unsigned)n);
}

/** @brief 长 UTF-8 输入经 app_render：输入夹到 text_buf，输出不越界且排版合理 */
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

/* ================================================================
 *  用例：RLS 显示帧位图长度校验（P6）
 * ================================================================ */

static app_ccb_t s_rls_ccb = {.name = "rls_ut", .ops = nullptr};

/**
 * @brief RLS 显示帧：位图不足 → 拒显（不调 app_render）；足够 → 正常调用
 *
 * 口径取 **≥**：`帧内位图可用 = data_len − 帧头6 − 显示头4 − BCC1 − 尾2 = data_len − 13`，
 * 只要 `≥ ceil(逻辑屏宽/8)×逻辑屏高` 就放行（多带的字节不用也不拒）。
 * 两条实际几何：3833024 单卡 128×32 需 512 B（一帧 530B 装得下）；
 * 5006048 级联 224×100 需 2800 B，而一帧最多 530 B —— **必然拒显**。
 *
 * 下界侧另钉 `app_rls_cmd.c:49-52` 的守卫（`data_len > overhead`）：`data_len ≤ 13`
 * 时可用位图必须是 0，否则 `data_len − 13` 在 uint16 下回绕、反被当成"位图很足"。
 */
static void case_rls_display_len(void)
{
    TEST_BEGIN("RLS 显示帧：位图不足 → 拒显（不调 app_render）；足够 → 正常调用");

    /* 所需字节的纯函数：钉住两板的实际数字 */
    CHECK_MSG(_rls_display_bitmap_bytes(128, 32) == 512, "3833024 128×32 应需 512 B，得到 %u",
              (unsigned)_rls_display_bitmap_bytes(128, 32));
    CHECK_MSG(_rls_display_bitmap_bytes(224, 100) == 2800, "5006048 224×100 应需 2800 B，得到 %u",
              (unsigned)_rls_display_bitmap_bytes(224, 100));

    static uint8_t s_frame_buf[600];
    app_rls_display_t *disp = (app_rls_display_t *)s_frame_buf;
    memset(s_frame_buf, 0, sizeof(s_frame_buf));
    disp->color = 0; /* 协议口径 0 = 红（handler 负责映射成 dev_display_color_t） */

    /* --- 3833024 几何：need = 512 B --- */
    s_stub_rows = 128;
    s_stub_cols = 32;

    /* --- 头部边界（守卫哨兵）：`data_len ≤ 13` = 帧头 6 + 显示头 4 + BCC 1 + 尾 2 ---
       本组钉的是 `app_rls_cmd.c:49-52` 的 `data_len > overhead ? data_len − 13 : 0`：
       守卫被拆掉的话 `data_len − 13` 会在 uint16 下**回绕**成 65523~65535，
       反而被当成"位图很足"放行 → `app_render` 按逻辑屏几何越界读帧缓冲。 */
    CHECK_MSG(_rls_display_bitmap_have(0) == 0 && _rls_display_bitmap_have(12) == 0 &&
                  _rls_display_bitmap_have(13) == 0,
              "data_len ≤ 13 时可用位图必须是 0（回绕即守卫失效）：得到 %u / %u / %u",
              (unsigned)_rls_display_bitmap_have(0), (unsigned)_rls_display_bitmap_have(12),
              (unsigned)_rls_display_bitmap_have(13));

    /* 头部 −1 / 恰好头部 / 空帧：三种都必须拒显（一次 app_render 都不许调） */
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 12);
    CHECK_MSG(s_rec_cnt == 0, "data_len=12（少 1 字节才够帧头）必须拒显：期望 0 次绘制，得到 %d",
              s_rec_cnt);

    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 13);
    CHECK_MSG(s_rec_cnt == 0, "data_len=13（恰好帧头、位图为 0）必须拒显：期望 0 次绘制，得到 %d",
              s_rec_cnt);

    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 0);
    CHECK_MSG(s_rec_cnt == 0, "data_len=0（空帧）必须拒显：期望 0 次绘制，得到 %d", s_rec_cnt);

    /* 短帧：整帧 64 B → 位图可用 64−13 = 51 < 512 → 拒显（一次 app_render 都不许调） */
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 64);
    CHECK_MSG(s_rec_cnt == 0, "位图不足时必须拒显：期望 0 次绘制，得到 %d", s_rec_cnt);

    /* 恰好装下 512 B 位图：整帧 = 13 + 512 = 525 ≤ RLS_PAYLOAD_MAX(530) → 清屏 + 贴图 */
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 525);
    CHECK_MSG(s_rec_cnt == 2, "位图足够时应清屏+贴图（2 次），得到 %d", s_rec_cnt);
    CHECK_MSG(count_bitmaps() == 1, "应恰好贴一次位图，得到 %d", count_bitmaps());

    /* 边界：差 1 字节 → 拒显（口径 ≥，不将就） */
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 524);
    CHECK_MSG(s_rec_cnt == 0, "位图差 1 B 应拒显（口径取 ≥），得到 %d", s_rec_cnt);

    /* --- 5006048 几何：need = 2800 B，一帧上限 530 B → 必然拒显 --- */
    s_stub_rows = 224;
    s_stub_cols = 100;
    cap_reset(224, 100);
    _rls_cmd_display(&s_rls_ccb, disp, 530); /* = RLS_PAYLOAD_MAX，帧能带的最大量 */
    CHECK_MSG(s_rec_cnt == 0,
              "5006048 逻辑屏 224×100 需 2800 B、一帧最多 530 B —— 必然拒显，得到 %d 次绘制",
              s_rec_cnt);

    /* 还原几何，免得影响后续用例 */
    s_stub_rows = 64;
    s_stub_cols = 64;
}

/* ================================================================ */

/**
 * @brief RLS 显示帧颜色：线上 0/1/2 → 红/绿/黄；表外值整帧拒显（不碰 app_render）
 *
 * 直传有两层后果：`dev_display_color_t` 是 BLACK=0/RED=1/GREEN=2/YELLOW=3，直传会把
 * "红"画成灭；更糟的是 >7 的值会一路落到板级扫描的 `s_bsrr_table[ch][color]`，
 * 越界读出来的 `port` 是垃圾指针 → 扫描中断里往任意地址写 BSRR。故本用例同时钉
 * "映射正确"与"表外拒显"两件事，并覆盖 SAVE 与 TMP 共用同一口径。
 */
static void case_rls_color(void)
{
    TEST_BEGIN("RLS 显示帧颜色：0/1/2 → 红/绿/黄，表外值拒显");

    static uint8_t s_color_buf[600];
    app_rls_display_t *disp = (app_rls_display_t *)s_color_buf;
    memset(s_color_buf, 0, sizeof(s_color_buf));

    s_stub_rows = 128;
    s_stub_cols = 32;

    /* 三色逐一映射：位图足够（525B）→ 清屏 + 贴图，贴图那一笔的颜色即映射结果 */
    static const struct {
        uint8_t             wire;
        dev_display_color_t expect;
    } cases[] = {
        {0, DEV_DISPLAY_COLOR_RED},
        {1, DEV_DISPLAY_COLOR_GREEN},
        {2, DEV_DISPLAY_COLOR_YELLOW},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        disp->color = cases[i].wire;
        cap_reset(128, 32);
        _rls_cmd_display(&s_rls_ccb, disp, 525);
        const uint8_t got = (s_rec_cnt == 2) ? s_recs[1].color : 0xEE;
        CHECK_MSG(s_rec_cnt == 2 && s_recs[1].is_bitmap && got == (uint8_t)cases[i].expect,
                  "color=%u 应映射成 %u 并贴图（期望 2 次绘制），得到 %d 次、颜色 %u",
                  (unsigned)cases[i].wire, (unsigned)cases[i].expect, s_rec_cnt, (unsigned)got);
    }

    /* 表外值：3（SV 那张表的"黄"）、7（枚举内但协议外）、0xFF —— 都必须整帧拒显 */
    const uint8_t bad[] = {3, 7, 0xFF};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        disp->color = bad[i];
        cap_reset(128, 32);
        _rls_cmd_display(&s_rls_ccb, disp, 525);
        CHECK_MSG(s_rec_cnt == 0, "color=%u 越界必须拒显：期望 0 次绘制，得到 %d", (unsigned)bad[i],
                  s_rec_cnt);
    }

    /* SAVE 与 TMP 必须共用同一口径（各写一份就会漂移） */
    disp->color = 0;
    cap_reset(128, 32);
    _rls_cmd_display_save(&s_rls_ccb, disp, 525);
    CHECK_MSG(s_rec_cnt == 2 && s_recs[1].color == (uint8_t)DEV_DISPLAY_COLOR_RED,
              "SAVE 也应把 0 映射成红并贴图，得到 %d 次绘制", s_rec_cnt);

    disp->color = 0xFF;
    cap_reset(128, 32);
    _rls_cmd_display_save(&s_rls_ccb, disp, 525);
    CHECK_MSG(s_rec_cnt == 0, "SAVE 遇到表外颜色也必须拒显，得到 %d 次绘制", s_rec_cnt);

    (void)app_render_take_persist_req(); /* 清掉 SAVE 置的落盘请求，别让后续用例看到 */

    s_stub_rows = 64;
    s_stub_cols = 64;
}

/* ================================================================ */

/**
 * @brief RLS 显示帧亮度：0~31 压到面板 1~7 档，255 恢复光感跟随，被拒的帧无副作用
 *
 * 口径来自文档：「亮度（32 级）0~31，0 最暗、31 最亮，255 自动」。面板只有 3 位
 * 亮度且 0 = 全灭，故 0~31 落 **1~7**（0 落 1）—— 关屏是 DISPLAY_SW 的事，不该由
 * 亮度字段顺手做掉。32~254 协议外，按最亮钳制。
 */
static void case_rls_light(void)
{
    TEST_BEGIN("RLS 显示帧亮度：0~31 → 1~7、255 → 恢复自动跟随");

    static uint8_t s_light_buf[600];
    app_rls_display_t *disp = (app_rls_display_t *)s_light_buf;
    memset(s_light_buf, 0, sizeof(s_light_buf));
    disp->color = 0; /* 红：先过颜色校验，才能走到亮度这一步 */

    s_stub_rows = 128;
    s_stub_cols = 32;

    static const struct {
        uint8_t wire;
        uint8_t hw;
    } cases[] = {
        {0, 1},   /* 协议最暗 → 硬件最暗的**非零**档 */
        {16, 4},
        {31, 7},
        {32, 7},  /* 协议外：按最亮钳制，不让显示失败 */
        {254, 7},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        s_light_fixed_cnt  = 0;
        s_light_resume_cnt = 0;
        disp->light_level  = cases[i].wire;
        cap_reset(128, 32);
        _rls_cmd_display(&s_rls_ccb, disp, 525);
        CHECK_MSG(s_light_fixed_cnt == 1 && s_light_fixed_last == cases[i].hw &&
                      s_light_resume_cnt == 0,
                  "亮度 %u 应设成 %u 档（固定 1 次 / 恢复 0 次），得到 fixed=%d(%u) resume=%d",
                  (unsigned)cases[i].wire, (unsigned)cases[i].hw, s_light_fixed_cnt,
                  (unsigned)s_light_fixed_last, s_light_resume_cnt);
    }

    /* 255 = 自动亮度：恢复光感跟随，不写固定档 */
    s_light_fixed_cnt  = 0;
    s_light_resume_cnt = 0;
    disp->light_level  = 255;
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 525);
    CHECK_MSG(s_light_resume_cnt == 1 && s_light_fixed_cnt == 0,
              "亮度 255 应恢复自动跟随（恢复 1 次、固定 0 次），得到 resume=%d fixed=%d",
              s_light_resume_cnt, s_light_fixed_cnt);

    /* 拒显的帧不得有副作用：位图不足时亮度也不许动 */
    s_light_fixed_cnt  = 0;
    s_light_resume_cnt = 0;
    disp->light_level  = 31;
    cap_reset(128, 32);
    _rls_cmd_display(&s_rls_ccb, disp, 64); /* 位图不足 → 拒显 */
    CHECK_MSG(s_light_fixed_cnt == 0 && s_light_resume_cnt == 0,
              "被拒的帧不得改亮度：得到 fixed=%d resume=%d", s_light_fixed_cnt, s_light_resume_cnt);

    /* SAVE 与 TMP 共用同一亮度口径 */
    s_light_fixed_cnt  = 0;
    s_light_resume_cnt = 0;
    disp->light_level  = 31;
    cap_reset(128, 32);
    _rls_cmd_display_save(&s_rls_ccb, disp, 525);
    CHECK_MSG(s_light_fixed_cnt == 1 && s_light_fixed_last == 7,
              "SAVE 也应应用亮度 31 → 7 档，得到 fixed=%d(%u)", s_light_fixed_cnt,
              (unsigned)s_light_fixed_last);
    (void)app_render_take_persist_req();

    s_stub_rows = 64;
    s_stub_cols = 64;
}

/* ================================================================ */

/**
 * @brief RLS 显示开关：0x31 开、0x30 关，只切 OE；其它值/短帧无副作用
 *
 * 开关与亮度是**两个状态**（CDSTD A0H/A2H 定下的口径）：拿"设亮度 0"实现关屏会把
 * 关屏前的档位永久丢掉。参考分支把 0x30/0x31 当亮度值喂给 _rls_apply_light_level()，
 * 两个值都落到"最亮 7"，关屏根本关不掉 —— 本用例把正确语义钉住。
 */
static void case_rls_display_sw(void)
{
    TEST_BEGIN("RLS 显示开关：0x31 开 / 0x30 关，只切 OE 不动亮度");

    /* 文档两处对命令码的写法不同：数值列 "55"（0x35 0x35）、参考分支用 0x35 0x55，
       两种都收（同一 handler），枚举值按线上两字节小端拼接 */
    CHECK_MSG((APP_RLS_CMD_TYPE_DISPLAY_SW & 0xFFU) == 0x35U &&
                  ((APP_RLS_CMD_TYPE_DISPLAY_SW >> 8) & 0xFFU) == 0x35U,
              "命令码 0x%04X 应为线上 0x35 0x35", (unsigned)APP_RLS_CMD_TYPE_DISPLAY_SW);
    CHECK_MSG((APP_RLS_CMD_TYPE_DISPLAY_SW_ALT & 0xFFU) == 0x35U &&
                  ((APP_RLS_CMD_TYPE_DISPLAY_SW_ALT >> 8) & 0xFFU) == 0x55U,
              "备用命令码 0x%04X 应为线上 0x35 0x55", (unsigned)APP_RLS_CMD_TYPE_DISPLAY_SW_ALT);

    static uint8_t s_sw_buf[16];
    memset(s_sw_buf, 0, sizeof(s_sw_buf));

    /* 0x31 = 打开显示 */
    s_oe_enable_cnt  = 0;
    s_oe_disable_cnt = 0;
    s_sw_buf[0]      = 0x31;
    _rls_cmd_display_sw(&s_rls_ccb, s_sw_buf, 10);
    CHECK_MSG(s_oe_enable_cnt == 1 && s_oe_disable_cnt == 0,
              "0x31 应开屏（enable=1 / disable=0），得到 %d / %d", s_oe_enable_cnt,
              s_oe_disable_cnt);

    /* 0x30 = 关闭显示 */
    s_oe_enable_cnt  = 0;
    s_oe_disable_cnt = 0;
    s_sw_buf[0]      = 0x30;
    _rls_cmd_display_sw(&s_rls_ccb, s_sw_buf, 10);
    CHECK_MSG(s_oe_enable_cnt == 0 && s_oe_disable_cnt == 1,
              "0x30 应关屏（enable=0 / disable=1），得到 %d / %d", s_oe_enable_cnt,
              s_oe_disable_cnt);

    /* 协议外参数：什么都不做（不能默认当成"开"） */
    s_oe_enable_cnt  = 0;
    s_oe_disable_cnt = 0;
    s_sw_buf[0]      = 0x32;
    _rls_cmd_display_sw(&s_rls_ccb, s_sw_buf, 10);
    CHECK_MSG(s_oe_enable_cnt == 0 && s_oe_disable_cnt == 0,
              "0x32 应被忽略（0/0），得到 %d / %d", s_oe_enable_cnt, s_oe_disable_cnt);

    /* 短帧（9 字节 < 10）：整条丢弃 */
    s_oe_enable_cnt  = 0;
    s_oe_disable_cnt = 0;
    s_sw_buf[0]      = 0x31;
    _rls_cmd_display_sw(&s_rls_ccb, s_sw_buf, 9);
    CHECK_MSG(s_oe_enable_cnt == 0 && s_oe_disable_cnt == 0,
              "9 字节短帧必须丢弃（0/0），得到 %d / %d", s_oe_enable_cnt, s_oe_disable_cnt);
}

/**
 * @brief RLS 修改 IP：12 字节载荷落 IAP 记录、端口沿用当前配置；短帧整条丢弃
 *
 * 协议帧里没有端口字段，**不能固定传 0** —— 那会把记录里的 port 覆盖掉。口径：记录
 * 有效取记录的端口，无记录（直烧板）才回落运行态 TCP 端口。
 */
static void case_rls_set_ip(void)
{
    TEST_BEGIN("RLS 修改 IP：写 IAP 记录并保留端口，短帧丢弃");

    /* 命令码口径：线上 0x49 0x50（"IP"）在这个枚举里是小端拼成的 0x5049 */
    CHECK_MSG((APP_RLS_CMD_TYPE_SET_IP & 0xFFU) == 0x49U &&
                  ((APP_RLS_CMD_TYPE_SET_IP >> 8) & 0xFFU) == 0x50U,
              "命令码应为线上 'IP' = 0x49 0x50，枚举 0x%04X", (unsigned)APP_RLS_CMD_TYPE_SET_IP);

    static uint8_t s_ip_buf[64];
    app_rls_net_cfg_t *cfg = (app_rls_net_cfg_t *)s_ip_buf;
    memset(s_ip_buf, 0, sizeof(s_ip_buf));
    const uint8_t ip[4]   = {10, 1, 2, 3};
    const uint8_t mask[4] = {255, 255, 255, 0};
    const uint8_t gw[4]   = {10, 1, 2, 1};
    memcpy(cfg->ip, ip, 4);
    memcpy(cfg->mask, mask, 4);
    memcpy(cfg->gw, gw, 4);

    /* 有记录：端口取记录里的 6001，而不是运行态 TCP 端口 */
    s_iap_rec_valid = true;
    s_iap_rec.port  = 6001;
    s_iap_upd_cnt   = 0;
    _rls_cmd_set_ip(&s_rls_ccb, s_ip_buf, 21);
    CHECK_MSG(s_iap_upd_cnt == 1, "21 字节帧应写 1 次记录，得到 %d 次", s_iap_upd_cnt);
    CHECK_MSG(memcmp(s_iap_new_ip, ip, 4) == 0 && memcmp(s_iap_new_mask, mask, 4) == 0 &&
                  memcmp(s_iap_new_gw, gw, 4) == 0,
              "记录里的地址应为 %u.%u.%u.%u/%u.%u.%u.%u/%u.%u.%u.%u", (unsigned)s_iap_new_ip[0],
              (unsigned)s_iap_new_ip[1], (unsigned)s_iap_new_ip[2], (unsigned)s_iap_new_ip[3],
              (unsigned)s_iap_new_mask[0], (unsigned)s_iap_new_mask[1],
              (unsigned)s_iap_new_mask[2], (unsigned)s_iap_new_mask[3], (unsigned)s_iap_new_gw[0],
              (unsigned)s_iap_new_gw[1], (unsigned)s_iap_new_gw[2], (unsigned)s_iap_new_gw[3]);
    CHECK_MSG(s_iap_new_port == 6001, "端口应沿用记录里的 6001，得到 %u", (unsigned)s_iap_new_port);

    /* 无记录：回落运行态 TCP 端口 */
    s_iap_rec_valid = false;
    s_iap_upd_cnt   = 0;
    _rls_cmd_set_ip(&s_rls_ccb, s_ip_buf, 21);
    CHECK_MSG(s_iap_upd_cnt == 1 && s_iap_new_port == s_tcp_port,
              "无记录时应回落 TCP 端口 %u，得到 upd=%d port=%u", (unsigned)s_tcp_port,
              s_iap_upd_cnt, (unsigned)s_iap_new_port);

    /* 短帧（20 字节 = 差 1 字节）必须整条丢弃，不得有副作用 */
    s_iap_upd_cnt = 0;
    _rls_cmd_set_ip(&s_rls_ccb, s_ip_buf, 20);
    CHECK_MSG(s_iap_upd_cnt == 0, "20 字节帧必须丢弃：期望 0 次写入，得到 %d", s_iap_upd_cnt);
}

/**
 * @brief RLS 搜索应答：21 字节 REPORT_IP 帧逐字节对拍（帧头/长度/命令/地址/BCC/帧尾）
 *
 * BCC 口径按文档正文"帧头到 BCC 之前所有字节异或"，即前 18 字节；参考分支只算 12
 * 字节数据域。若现场上位机校验不过，改这一处的长度即可定位。
 */
static void case_rls_report_ip(void)
{
    TEST_BEGIN("RLS 搜索应答：21 字节 REPORT_IP，BCC 覆盖帧头到数据域");

    s_tx_cnt = 0;
    s_tx_len = 0;
    _rls_cmd_aquire_ip(&s_rls_ccb, nullptr, 0);

    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == (int)RLS_REPORT_IP_LEN,
              "应回 1 帧 %u 字节，得到 %d 帧 %d 字节", (unsigned)RLS_REPORT_IP_LEN, s_tx_cnt,
              s_tx_len);
    CHECK_MSG(s_tx_buf[0] == 0xFF && s_tx_buf[1] == 0xFE, "帧头应为 FF FE，得到 %02X %02X",
              (unsigned)s_tx_buf[0], (unsigned)s_tx_buf[1]);
    CHECK_MSG(s_tx_buf[2] == 0x00 && s_tx_buf[3] == RLS_REPORT_IP_LEN,
              "长度域应为大端 0x00 0x15，得到 %02X %02X", (unsigned)s_tx_buf[2],
              (unsigned)s_tx_buf[3]);
    CHECK_MSG(s_tx_buf[4] == 0x52 && s_tx_buf[5] == 0x42, "命令码应为线上 'RB' = 0x52 0x42，得到 %02X %02X",
              (unsigned)s_tx_buf[4], (unsigned)s_tx_buf[5]);
    CHECK_MSG(memcmp(s_tx_buf + 6, s_net_ip, 4) == 0 && memcmp(s_tx_buf + 10, s_net_mask, 4) == 0 &&
                  memcmp(s_tx_buf + 14, s_net_gw, 4) == 0,
              "地址域应为运行态 IP/掩码/网关");

    uint8_t bcc = 0;
    for (int i = 0; i < 18; i++) bcc ^= s_tx_buf[i];
    CHECK_MSG(s_tx_buf[18] == bcc, "BCC 应为前 18 字节异或 0x%02X，得到 0x%02X", (unsigned)bcc,
              (unsigned)s_tx_buf[18]);
    CHECK_MSG(s_tx_buf[19] == 0x0D && s_tx_buf[20] == 0x0C, "帧尾应为 0D 0C，得到 %02X %02X",
              (unsigned)s_tx_buf[19], (unsigned)s_tx_buf[20]);
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

    /* 建模块互斥量（host 上 sw_app_initcall 不执行，直接调用 production 的 _render_init） */
    _render_init();

    /* app_render 模块锁：每次调用恰好一次 Acquire/Release（含提前返回） */
    case_render_lock_balance();

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
    case_prefer_one_line();
    case_adaptive_multiline();
    case_cvt_mixed();
    case_cvt_truncated();
    case_cvt_unicode_bounds();
    case_long_utf8_clamp();
    case_rls_display_len();
    case_rls_color();
    case_rls_light();
    case_rls_display_sw();
    case_rls_set_ip();
    case_rls_report_ip();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
