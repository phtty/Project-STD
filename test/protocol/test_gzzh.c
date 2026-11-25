/**
 * @file    test_gzzh.c
 * @brief   GZZH（贵州地标情报板精简协议）host 单测：命令分派矩阵与各命令行为
 *
 * 帧探测（引导串 / 长度域上下界 / 结束符）在 `test_probes.c` 里测，本套件不重复；
 * 这里从"一帧完整帧"往里测：调 `app_gzzh_handle_frame()`，看**真命令表**分派到哪个
 * 处理函数、回了什么、对渲染 / IAP 记录做了什么。
 *
 * **为什么需要这个测试**：
 *   1. 命令表是"命令字 → 处理函数"的唯一分派路径，错一处就是命令 A 干了命令 B 的事；
 *   2. 线上字段的语义几乎都没有抓包可对照（字体名 GBK 四字节、字号编码、颜色优先级、
 *      端口大端），只能靠"参考实现口径 + 文档"钉死，将来漂移必须立刻变红；
 *   3. 三个数值域各自对应一类现场故障：颜色越界会传到扫描驱动、坐标越界会让排版算到
 *      屏外、载荷长度不足会读到帧外。
 *
 * 手法：生产 TU（`app_gzzh.c` / `app_gzzh_cmd.c`）**照原样编译**（不 TU-include），
 * 副作用用记录桩观察（渲染 / IAP 记录 / 通道发送）；帧由用例按**帧结构体字段**构造。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_gzzh.h"
#include "app_gzzh_cmd.h"
#include "app_iap_cfg.h"
#include "app_render.h"
#include "app_screen.h"

/* ================================================================
 *  断言（附录 A 的 A 型）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  记录桩
 * ================================================================ */

/* ---- 通道发送（0x10 裸 2 字节 / 0x40 的 0x50 应答都走它）---- */
static int      s_tx_cnt;
static uint16_t s_tx_len;
static uint8_t  s_tx_buf[64];

int32_t app_ccb_send(app_ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    s_tx_cnt++;
    s_tx_len = len;
    if (len <= sizeof(s_tx_buf)) memcpy(s_tx_buf, data, len);
    return (int32_t)len;
}

/* ---- 渲染：按 type 分支读 union 字段（§4.1 第 5 条）---- */
typedef struct {
    uint8_t  type;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t  color;
    bool     persist;

    /* 仅 TEXT 分支有效 */
    uint16_t text_len;
    uint8_t  font_size;
    uint8_t  font_type;
    uint8_t  text_enc;
    uint8_t  h_align;
    uint8_t  v_align;
    char     text[16];
} draw_t;

#define DRAW_MAX (8U)
static draw_t s_draws[DRAW_MAX];
static int    s_draw_cnt;

void app_render(const app_render_cfg_t *cfg)
{
    if (s_draw_cnt >= (int)DRAW_MAX) return;

    draw_t *d = &s_draws[s_draw_cnt++];
    memset(d, 0, sizeof(*d));

    d->type    = (uint8_t)cfg->type;
    d->x       = cfg->x;
    d->y       = cfg->y;
    d->w       = cfg->w;
    d->h       = cfg->h;
    d->color   = (uint8_t)cfg->color;
    d->persist = cfg->persist;

    if (cfg->type != APP_RENDER_TYPE_TEXT) return;

    d->text_len  = cfg->len;
    d->font_size = (uint8_t)cfg->font_size;
    d->font_type = (uint8_t)cfg->font_type;
    d->text_enc  = (uint8_t)cfg->text_enc;
    if (cfg->style != nullptr) {
        d->h_align = (uint8_t)cfg->style->h_align;
        d->v_align = (uint8_t)cfg->style->v_align;
    }
    if ((cfg->text != nullptr) && (cfg->len < sizeof(d->text)))
        memcpy(d->text, cfg->text, cfg->len);
}

/* ---- 逻辑屏几何（3833024 口径：128×32）---- */
static uint16_t s_rows = 128;
static uint16_t s_cols = 32;

uint16_t app_screen_rows(void) { return s_rows; }
uint16_t app_screen_cols(void) { return s_cols; }

/* ---- IAP 网络记录 ---- */
static uint8_t  s_net_ip[4];
static uint8_t  s_net_mask[4];
static uint8_t  s_net_gw[4];
static uint32_t s_net_port;
static int      s_net_upd_cnt;

void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    s_net_upd_cnt++;
    memcpy(s_net_ip, ip, 4);
    memcpy(s_net_mask, mask, 4);
    memcpy(s_net_gw, gw, 4);
    s_net_port = port;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static app_ccb_t s_ccb = {.name = "gzzh_ut", .ops = nullptr};

/** @brief 清空所有记录与几何（每个用例开头调一次） */
static void fixture_reset(void)
{
    s_tx_cnt = 0;
    s_tx_len = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));

    s_draw_cnt = 0;
    memset(s_draws, 0, sizeof(s_draws));

    s_net_upd_cnt = 0;
    memset(s_net_ip, 0, 4);
    memset(s_net_mask, 0, 4);
    memset(s_net_gw, 0, 4);
    s_net_port = 0;

    s_rows = 128;
    s_cols = 32;
}

/* ---- 帧构造 ---- */
static uint8_t s_frame[GZZH_FRAME_MAX_BYTES];

/** @brief 按字段构造一整帧（长度域 = 整帧字节数，含引导串与结束符）
 *  @return 整帧字节数 */
static uint16_t frame_build(app_gzzh_cmd_type_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    const uint16_t total = (uint16_t)(sizeof(app_gzzh_frame_t) + payload_len + 1U);
    app_gzzh_frame_t *f  = (app_gzzh_frame_t *)s_frame;

    memset(s_frame, 0, sizeof(s_frame));
    memcpy(f->head, g_gzzh_head, GZZH_HEAD_BYTES);
    f->length[0] = (uint8_t)(total & 0xFFU);
    f->length[1] = (uint8_t)((total >> 8) & 0xFFU);
    f->cmd[0]    = (uint8_t)cmd;

    if ((payload != nullptr) && (payload_len > 0)) memcpy(f->payload, payload, payload_len);
    s_frame[total - 1] = GZZH_TAIL_BYTE;
    return total;
}

/* ---- 0x20 载荷构造 ---- */
static uint8_t s_payload[1024];

/* GBK 字体名（与 app_gzzh_cmd.c 的映射表同一份字节，独立抄一遍当 oracle） */
static const uint8_t s_font_song[4] = {0xCB, 0xCE, 0xCC, 0xE5}; /* 宋体 */
static const uint8_t s_font_hei[4]  = {0xBA, 0xDA, 0xCC, 0xE5}; /* 黑体 */
static const uint8_t s_font_kai[4]  = {0xBF, 0xAC, 0xCC, 0xE5}; /* 楷体 */
static const uint8_t s_font_fang[4] = {0xB7, 0xC2, 0xCB, 0xCE}; /* 仿宋 */

/** @brief 构造 0x20 的基本数据包；`size_code` 写进字节 0
 *  @return 载荷字节数（17 + 文本长度） */
static uint16_t text_payload(uint16_t x, uint16_t y, const uint8_t font[4], uint8_t size_code,
                             const uint8_t rgb[3], const char *text, uint16_t text_len)
{
    app_gzzh_text_t *p = (app_gzzh_text_t *)s_payload;
    memset(s_payload, 0, sizeof(app_gzzh_text_t) + text_len);

    p->x[0] = (uint8_t)(x & 0xFFU);
    p->x[1] = (uint8_t)(x >> 8);
    p->y[0] = (uint8_t)(y & 0xFFU);
    p->y[1] = (uint8_t)(y >> 8);
    /* 屏宽/屏高：主机自报 128×32，本机不使用 */
    p->screen_w[0] = 0x80;
    p->screen_h[0] = 0x20;
    memcpy(p->font_name, font, 4);
    p->font_size[0] = size_code;
    memcpy(p->color, rgb, 3);
    if ((text != nullptr) && (text_len > 0)) memcpy(p->text, text, text_len);

    return (uint16_t)(sizeof(app_gzzh_text_t) + text_len);
}

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 0x10 的应答是**裸 2 字节**（参考实现口径）
 *
 *  这条最容易照文档实现成"完整帧"。多一个字节头或尾，上位机按 2 字节读就会错位，
 *  现场表现为"查故障永远没响应"。 */
static void case_status(void)
{
    TEST_BEGIN("0x10 故障查询：应答是裸 2 字节");

    fixture_reset();
    const uint16_t n = frame_build(APP_GZZH_CMD_TYPE_STATUS, nullptr, 0);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_tx_cnt == 1, "应发 1 帧，实际 %d", s_tx_cnt);
    CHECK_MSG(s_tx_len == GZZH_STATUS_BYTES, "应是裸 %u 字节，实际 %u", (unsigned)GZZH_STATUS_BYTES,
              (unsigned)s_tx_len);
    CHECK_MSG((s_tx_buf[0] == 0x00) && (s_tx_buf[1] == 0x00), "无故障应为 00 00，实际 %02X %02X",
              (unsigned)s_tx_buf[0], (unsigned)s_tx_buf[1]);
    CHECK_MSG(s_draw_cnt == 0, "故障查询不该动屏，实际画了 %d 笔", s_draw_cnt);
}

/** @brief 0x20 是"整屏替换"：先清整屏，再从 (x,y) 按字号/字型/颜色/GBK 渲染
 *
 *  任一字段错位现场都是可见故障：坐标错 → 字跑到别处；字号/字型错 → 字不对；
 *  颜色错 → 该红的显示成绿；persist 错 → 掉电后黑屏。 */
static void case_text(void)
{
    TEST_BEGIN("0x20 文字显示：清屏 + 逐字段下传");

    fixture_reset();
    const uint8_t rgb[3] = {0xFF, 0x00, 0x00};
    const uint16_t plen  = text_payload(8, 4, s_font_song, 24, rgb, "AB", 2);
    const uint16_t n     = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_draw_cnt == 2, "应是清屏 + 文字两笔，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].type == APP_RENDER_TYPE_FILL, "第一笔应是清屏");
    CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "清屏应铺满逻辑屏，实际 %ux%u",
              (unsigned)s_draws[0].w, (unsigned)s_draws[0].h);
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏应为黑，实际 %u",
              (unsigned)s_draws[0].color);
    CHECK_MSG(!s_draws[0].persist, "清屏不得落盘");

    const draw_t *t = &s_draws[1];
    CHECK_MSG(t->type == APP_RENDER_TYPE_TEXT, "第二笔应是文字");
    CHECK_MSG((t->x == 8) && (t->y == 4), "坐标应为 (8,4)，实际 (%u,%u)", (unsigned)t->x,
              (unsigned)t->y);
    CHECK_MSG((t->w == 120) && (t->h == 28), "区域应到屏边缘，实际 %ux%u", (unsigned)t->w,
              (unsigned)t->h);
    CHECK_MSG(t->color == DEV_DISPLAY_COLOR_RED, "红字应为 RED，实际 %u", (unsigned)t->color);
    CHECK_MSG(t->font_size == APP_FONT_SIZE_24, "字号 24，实际 %u", (unsigned)t->font_size);
    CHECK_MSG(t->font_type == APP_FONT_TYPE_HT, "宋体按参考实现显示黑体，实际 %u",
              (unsigned)t->font_type);
    CHECK_MSG(t->text_enc == APP_FONT_ENC_GBK, "文本按 GBK 直通，实际 %u", (unsigned)t->text_enc);
    CHECK_MSG((t->text_len == 2) && (0 == memcmp(t->text, "AB", 2)), "文本长度/内容应原样下传");
    CHECK_MSG((t->h_align == APP_RENDER_ALIGN_LEFT_UP) && (t->v_align == APP_RENDER_ALIGN_LEFT_UP),
              "顶格左对齐（协议没有对齐字段）");
    CHECK_MSG(t->persist, "显示内容要落盘（上电恢复）");
    CHECK_MSG(s_tx_cnt == 0, "显示命令无应答帧，实际发了 %d 帧", s_tx_cnt);
}

/** @brief 字体名映射：**照抄参考实现的倒置判断**（宋体→黑体，其余全落宋体）
 *
 *  参考实现是现场跑过的代码，线上口径以它为准：它把 `memcmp` 的返回值直接当条件
 *  （不相等时成立），于是"宋体"显示成黑体、其余全显示成宋体，"楷体"那一支不可达。
 *  这条是**现状哨兵**：将来若现场改成按名字正确映射，这条必须同步改红。
 *  判错的表现是"发楷体/仿宋，屏上是宋体"——现场会觉得字型选不动。 */
static void case_text_font_map(void)
{
    TEST_BEGIN("0x20 字体名 → 字型（参考实现口径，含倒置）");

    static const struct {
        const uint8_t  *name;
        app_font_type_t type;
        const char     *what;
    } k_cases[] = {
        {s_font_song, APP_FONT_TYPE_HT, "宋体（参考实现显示黑体）"},
        {s_font_hei, APP_FONT_TYPE_ST, "黑体（参考实现显示宋体）"},
        {s_font_kai, APP_FONT_TYPE_ST, "楷体（参考实现显示宋体）"},
        {s_font_fang, APP_FONT_TYPE_ST, "仿宋（参考实现显示宋体）"},
    };
    const uint8_t rgb[3] = {0xFF, 0x00, 0x00};

    for (uint8_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fixture_reset();
        const uint16_t plen =
            text_payload(0, 0, k_cases[i].name, 24, rgb, "A", 1);
        const uint16_t n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
        app_gzzh_handle_frame(&s_ccb, s_frame, n);
        CHECK_MSG(s_draw_cnt == 2 && s_draws[1].font_type == k_cases[i].type,
                  "%s 应映射成字型 %u，实际 %u", k_cases[i].what, (unsigned)k_cases[i].type,
                  (unsigned)(s_draw_cnt == 2 ? s_draws[1].font_type : 0xFF));
    }

    /* 未知字体名（"AAAA"）→ 宋体（参考实现里落在第一个分支） */
    fixture_reset();
    const uint8_t unknown[4] = {0x41, 0x41, 0x41, 0x41};
    const uint16_t plen      = text_payload(0, 0, unknown, 24, rgb, "A", 1);
    const uint16_t n         = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 2 && s_draws[1].font_type == APP_FONT_TYPE_ST, "未知字体应落宋体，实际 %u",
              (unsigned)(s_draw_cnt == 2 ? s_draws[1].font_type : 0xFF));
}

/** @brief 字号编码：像素高（16/24/32/48）与 0~3 序号两种读法都要认
 *
 *  参考实现 `switch (font_size[0])` 比对的是没给出数值的常量，现场到底是哪一种
 *  只能靠双兼容兜住；不认识的码按 16 号显示（静默按别的字号显示会让"字很小"查不出原因）。 */
static void case_text_size_map(void)
{
    TEST_BEGIN("0x20 字号码 → 字号");

    static const struct {
        uint8_t         code;
        app_font_size_t size;
    } k_cases[] = {
        {16, APP_FONT_SIZE_16}, {24, APP_FONT_SIZE_24}, {32, APP_FONT_SIZE_32},
        {48, APP_FONT_SIZE_48}, {0, APP_FONT_SIZE_16},  {1, APP_FONT_SIZE_24},
        {2, APP_FONT_SIZE_32},  {3, APP_FONT_SIZE_48},
    };
    const uint8_t rgb[3] = {0xFF, 0x00, 0x00};

    for (uint8_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fixture_reset();
        const uint16_t plen = text_payload(0, 0, s_font_song, k_cases[i].code, rgb, "A", 1);
        const uint16_t n    = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
        app_gzzh_handle_frame(&s_ccb, s_frame, n);
        CHECK_MSG(s_draw_cnt == 2 && s_draws[1].font_size == k_cases[i].size,
                  "字号 %u 应映射成 %u，实际 %u", (unsigned)k_cases[i].code,
                  (unsigned)k_cases[i].size,
                  (unsigned)(s_draw_cnt == 2 ? s_draws[1].font_size : 0xFF));
    }

    /* 不认识的码 → 16 号 */
    fixture_reset();
    const uint16_t plen = text_payload(0, 0, s_font_song, 99, rgb, "A", 1);
    const uint16_t n    = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 2 && s_draws[1].font_size == APP_FONT_SIZE_16, "未知字号码应回落 16 号");
}

/** @brief RGB → 面板色：**R 优先、其次 G、最后 B**（参考实现口径，红+绿给红不是黄）
 *
 *  这是与"2R1G 面板按组合取色"相反的口径；现场若要求 (255,255,0) 出黄，
 *  这条会红，届时按产品决定改实现与期望。 */
static void case_text_color_map(void)
{
    TEST_BEGIN("0x20 颜色字节 → 面板色");

    static const struct {
        uint8_t rgb[3];
        uint8_t color;
        const char *what;
    } k_cases[] = {
        {{0xFF, 0x00, 0x00}, DEV_DISPLAY_COLOR_RED, "红"},
        {{0x00, 0xFF, 0x00}, DEV_DISPLAY_COLOR_GREEN, "绿"},
        {{0x00, 0x00, 0xFF}, DEV_DISPLAY_COLOR_BLUE, "蓝"},
        {{0x00, 0x00, 0x00}, DEV_DISPLAY_COLOR_BLACK, "黑"},
        {{0xFF, 0xFF, 0x00}, DEV_DISPLAY_COLOR_RED, "红+绿（R 优先）"},
    };

    for (uint8_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fixture_reset();
        const uint16_t plen = text_payload(0, 0, s_font_song, 24, k_cases[i].rgb, "A", 1);
        const uint16_t n    = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, plen);
        app_gzzh_handle_frame(&s_ccb, s_frame, n);
        CHECK_MSG(s_draw_cnt == 2 && s_draws[1].color == k_cases[i].color, "%s 应为色 %u，实际 %u",
                  k_cases[i].what, (unsigned)k_cases[i].color,
                  (unsigned)(s_draw_cnt == 2 ? s_draws[1].color : 0xFF));
    }
}

/** @brief 0x20 的越界矩阵：坐标出屏 / 载荷不足 / 空文本 —— 一律**零副作用**
 *
 *  坐标越界时若先清屏再判，一条错命令就会擦掉整屏（现场表现为"屏忽然黑了"）；
 *  载荷不足若继续解析，文本会从帧外的字节里读。
 *  注释里另钉一条现状：**纯黑文字按清屏口径不落盘**。 */
static void case_text_bounds(void)
{
    TEST_BEGIN("0x20 越界矩阵：零副作用");

    const uint8_t rgb[3] = {0xFF, 0x00, 0x00};
    uint16_t      n;

    /* 载荷恰好 = 固定头（没有文本）→ 不画 */
    fixture_reset();
    memset(s_payload, 0, sizeof(app_gzzh_text_t));
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, (uint16_t)sizeof(app_gzzh_text_t));
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "空文本不该动屏，实际 %d 笔", s_draw_cnt);

    /* 载荷不足一个固定头 → 不画 */
    fixture_reset();
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, (uint16_t)(sizeof(app_gzzh_text_t) - 1U));
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "载荷不足不该动屏，实际 %d 笔", s_draw_cnt);

    /* x 恰好等于屏宽（越界边界）→ 不画（且不得先清屏） */
    fixture_reset();
    const uint16_t x_payload_len = text_payload(128, 0, s_font_song, 24, rgb, "A", 1);
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, x_payload_len);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "x=屏宽 应拒绝，实际 %d 笔", s_draw_cnt);

    /* y 恰好等于屏高 → 不画 */
    fixture_reset();
    const uint16_t y_payload_len = text_payload(0, 32, s_font_song, 24, rgb, "A", 1);
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, y_payload_len);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "y=屏高 应拒绝，实际 %d 笔", s_draw_cnt);

    /* 纯黑文字：清屏 + 画（屏幕结果就是黑），但**不落盘**（否则掉电重启黑屏） */
    fixture_reset();
    const uint8_t black[3] = {0x00, 0x00, 0x00};
    const uint16_t black_len = text_payload(0, 0, s_font_song, 24, black, "A", 1);
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, black_len);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 2, "黑字仍然画（清屏 + 文字），实际 %d 笔", s_draw_cnt);
    CHECK_MSG(!s_draws[1].persist, "纯黑文本不该落盘");
}

/** @brief 0x30 清屏：整屏填黑、不落盘、无应答 */
static void case_clear(void)
{
    TEST_BEGIN("0x30 清屏");

    fixture_reset();
    const uint16_t n = frame_build(APP_GZZH_CMD_TYPE_CLEAR, nullptr, 0);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_draw_cnt == 1, "应是 1 笔清屏，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].type == APP_RENDER_TYPE_FILL, "清屏应是填充");
    CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "应铺满逻辑屏");
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "应为黑");
    CHECK_MSG(!s_draws[0].persist, "清屏不得落盘");
    CHECK_MSG(s_tx_cnt == 0, "清屏无应答，实际发了 %d 帧", s_tx_cnt);
}

/** @brief 0x80 全屏纯色（文档未列、参考实现有）：RGB 铺满整屏，纯黑等同清屏不落盘 */
static void case_fill(void)
{
    TEST_BEGIN("0x80 全屏纯色");

    const uint8_t green[3] = {0x00, 0xFF, 0x00};
    fixture_reset();
    uint16_t n = frame_build(APP_GZZH_CMD_TYPE_FILL, green, GZZH_FILL_BYTES);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 1, "应是 1 笔填充，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_GREEN, "绿色，实际 %u",
              (unsigned)s_draws[0].color);
    CHECK_MSG(s_draws[0].persist, "纯色显示要落盘");

    const uint8_t black[3] = {0x00, 0x00, 0x00};
    fixture_reset();
    n = frame_build(APP_GZZH_CMD_TYPE_FILL, black, GZZH_FILL_BYTES);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 1 && !s_draws[0].persist, "纯黑等同清屏，不得落盘");

    /* 载荷不足 3 字节 → 零副作用 */
    fixture_reset();
    n = frame_build(APP_GZZH_CMD_TYPE_FILL, green, GZZH_FILL_BYTES - 1U);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "载荷不足不该动屏，实际 %d 笔", s_draw_cnt);
}

/** @brief 0x40 设置 IP：写 IAP 记录（**端口大端**）+ 回 0x50 应答帧
 *
 *  端口是这帧里唯一的大端字段；写反了上位机读回的是 4096 而不是 10000。 */
static void case_set_ip(void)
{
    TEST_BEGIN("0x40 设置 IP：记录 + 0x50 应答");

    fixture_reset();
    uint8_t payload[sizeof(app_gzzh_net_cfg_t)] = {0};
    payload[0] = 192;
    payload[1] = 168;
    payload[2] = 0;
    payload[3] = 220;
    payload[4] = 255;
    payload[5] = 255;
    payload[6] = 255;
    payload[7] = 0;
    payload[8]  = 192;
    payload[9]  = 168;
    payload[10] = 0;
    payload[11] = 1;
    payload[12] = 0x27; /* 端口 10000 的高字节（大端） */
    payload[13] = 0x10;

    const uint16_t n = frame_build(APP_GZZH_CMD_TYPE_SET_IP, payload, sizeof(payload));
    app_gzzh_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_net_upd_cnt == 1, "应写 1 次 IAP 记录，实际 %d", s_net_upd_cnt);
    CHECK_MSG((s_net_ip[0] == 192) && (s_net_ip[3] == 220), "IP 应原样写入");
    CHECK_MSG(s_net_mask[3] == 0, "掩码应原样写入");
    CHECK_MSG(s_net_gw[3] == 1, "网关应原样写入");
    CHECK_MSG(s_net_port == 10000U, "端口按大端拼，应为 10000，实际 %u", (unsigned)s_net_port);

    CHECK_MSG(s_tx_cnt == 1, "应回 1 帧应答，实际 %d", s_tx_cnt);
    CHECK_MSG(s_tx_len == GZZH_REPORT_IP_BYTES, "应答应是 %u 字节，实际 %u",
              (unsigned)GZZH_REPORT_IP_BYTES, (unsigned)s_tx_len);

    const app_gzzh_frame_t *r = (const app_gzzh_frame_t *)s_tx_buf;
    CHECK_MSG(0 == memcmp(r->head, g_gzzh_head, GZZH_HEAD_BYTES), "应答引导串应与请求一致");
    const uint16_t rlen = (uint16_t)((uint16_t)r->length[0] | ((uint16_t)r->length[1] << 8));
    CHECK_MSG(rlen == GZZH_REPORT_IP_BYTES, "应答长度域应是整帧字节数，实际 %u", (unsigned)rlen);
    CHECK_MSG(r->cmd[0] == (uint8_t)APP_GZZH_CMD_TYPE_REPORT_IP && r->cmd[1] == 0,
              "应答命令字应为 0x50，实际 %02X", (unsigned)r->cmd[0]);
    CHECK_MSG(0 == memcmp(r->payload, payload, sizeof(payload)), "应答载荷应原样回带");
    CHECK_MSG(s_tx_buf[GZZH_REPORT_IP_BYTES - 1] == GZZH_TAIL_BYTE, "应答结尾应是 0x00");

    /* 载荷不足 14 字节 → 不写记录、不应答 */
    fixture_reset();
    const uint16_t n2 =
        frame_build(APP_GZZH_CMD_TYPE_SET_IP, payload, (uint16_t)(sizeof(payload) - 1U));
    app_gzzh_handle_frame(&s_ccb, s_frame, n2);
    CHECK_MSG(s_net_upd_cnt == 0 && s_tx_cnt == 0, "载荷不足应零副作用");
}

/** @brief 命令分派：未知命令字、高字节非 0 —— 整帧丢弃且零副作用
 *
 *  高字节非 0 必须拦：参考实现按整 4 字节比对命令字，若这里只看低 8 位，
 *  一个 0x20000020 会被当成 0x20 执行。 */
static void case_dispatch(void)
{
    TEST_BEGIN("命令分派：未知 / 高字节非 0");

    fixture_reset();
    uint16_t n = frame_build((app_gzzh_cmd_type_t)0x11, nullptr, 0);
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_tx_cnt == 0) && (s_draw_cnt == 0) && (s_net_upd_cnt == 0),
              "未知命令应零副作用");

    /* 先按 0x20 造一帧，再把命令字的第 2 字节改成 0x01（线上 20 01 00 00） */
    fixture_reset();
    const uint8_t rgb[3] = {0xFF, 0x00, 0x00};
    const uint16_t text_len = text_payload(0, 0, s_font_song, 24, rgb, "A", 1);
    n = frame_build(APP_GZZH_CMD_TYPE_TEXT, s_payload, text_len);
    ((app_gzzh_frame_t *)s_frame)->cmd[1] = 0x01;
    app_gzzh_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_tx_cnt == 0) && (s_draw_cnt == 0), "命令字高字节非 0 应整帧丢弃");

    /* 命令表只含主机→设备方向：0x50（应答）不得在处理表里；五条命令各一次 */
    uint8_t mask = 0;
    for (uint8_t i = 0; i < GZZH_CMD_CNT; i++) {
        switch (g_gzzh_cmd_table[i].cmd) {
            case APP_GZZH_CMD_TYPE_STATUS: mask |= 0x01U; break;
            case APP_GZZH_CMD_TYPE_TEXT: mask |= 0x02U; break;
            case APP_GZZH_CMD_TYPE_CLEAR: mask |= 0x04U; break;
            case APP_GZZH_CMD_TYPE_SET_IP: mask |= 0x08U; break;
            case APP_GZZH_CMD_TYPE_FILL: mask |= 0x10U; break;
            case APP_GZZH_CMD_TYPE_REPORT_IP: mask |= 0x80U; break;
            default: mask |= 0x40U; break;
        }
    }
    CHECK_MSG(0U == (mask & 0x80U), "0x50 是设备→主机方向，不该进处理表");
    CHECK_MSG(mask == 0x1FU, "5 条命令应各出现一次（mask=%02X）", (unsigned)mask);
}

/* ================================================================
 *  main
 * ================================================================ */

int main(void)
{
    case_status();
    case_text();
    case_text_font_map();
    case_text_size_map();
    case_text_color_map();
    case_text_bounds();
    case_clear();
    case_fill();
    case_set_ip();
    case_dispatch();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
