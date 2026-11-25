/**
 * @file    test_p25rls.c
 * @brief   P25RLS（P25 可变信息板简易通讯协议）host 单测：命令分派矩阵与各命令行为
 *
 * 帧探测（帧头 / 长度域上下界 / 帧尾）在 `test_probes.c` 里测，本套件不重复；这里从
 * "一帧完整帧"往里测：调 `app_p25rls_handle_frame()`，看**真命令表**分派到哪个处理
 * 函数、回了什么、对渲染 / 亮度 / IAP 记录 / 图片槽做了什么。
 *
 * **为什么需要这个测试**：
 *   1. 命令字是两个 ASCII 字符按**小端拼成 uint16**（`"SV"` → 0x5653），拼错或按大端
 *      读就是"一条命令都不响应"，而这在线上抓包里看不出来（字节是对的）；
 *   2. `SV` 的位图是**红/绿两个平面、每像素 1bit**（协议文档正文写"1 点 1 byte"、
 *      同一张表又给 576B，按后者），平面顺序/长度错一位就是整屏错位；
 *   3. `SC` 的应答帧是**逐字节的线上契约**（长度域大端 21、BCC 覆盖前 18 字节），
 *      错一个字节上位机就认不出；
 *   4. `IP` 命令**没有端口字段**，端口必须沿用记录里的值 —— 顺手写成 0 会让设备
 *      重启后再也连不上（现场只能拆机）；
 *   5. 亮度 0~31 → 面板 1~7 的映射、255 = 恢复自动，是"屏太暗/太亮"类投诉的唯一来源。
 *
 * 手法：生产 TU（`app_p25rls.c` / `app_p25rls_cmd.c`）**照原样编译**（不 TU-include），
 * 副作用用记录桩观察（渲染 / 亮度 / IAP 记录 / 图片槽 / 通道发送）；帧由用例按
 * **帧结构体字段**构造。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_p25rls.h"
#include "app_p25rls_cmd.h"
#include "app_p25rls_pic.h"
#include "app_render.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "bcc_utils.h"
#include "pl_net.h"

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

/* ---- 通道发送（只有 SC 的 RB 应答走它）---- */
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

/* ---- 亮度 ---- */
static int     s_fixed_cnt;
static uint8_t s_fixed_last;
static int     s_resume_cnt;

void app_light_sensor_set_fixed(uint8_t level)
{
    s_fixed_cnt++;
    s_fixed_last = level;
}
void app_light_sensor_resume(void) { s_resume_cnt++; }

/* ---- 运行态网络（与记录里的值故意不同，好区分 SC 回的是哪一个）---- */
static uint8_t s_run_ip[4]   = {10, 20, 30, 40};
static uint8_t s_run_mask[4] = {255, 255, 255, 0};
static uint8_t s_run_gw[4]   = {10, 20, 30, 1};

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_run_ip, 4);
    memcpy(mask, s_run_mask, 4);
    memcpy(gw, s_run_gw, 4);
}

/* ---- IAP 记录 ---- */
static app_flash_iap_net_cfg_t s_iap_rec;
static bool                    s_iap_rec_valid;
static uint8_t                 s_iap_new_ip[4];
static uint8_t                 s_iap_new_mask[4];
static uint8_t                 s_iap_new_gw[4];
static uint32_t                s_iap_new_port;
static int                     s_iap_upd_cnt;
static uint16_t                s_tcp_port = 10011;

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

/* ---- 图片槽 ---- */
static int     s_pic_save_cnt;
static uint8_t s_pic_save_slot;
static uint8_t s_pic_save_bitmap[P25RLS_BITMAP_BYTES];
static int     s_pic_show_cnt;
static uint8_t s_pic_show_slot;
static int     s_order; /* 1 = save 过，2 = show 过（校验"先存后显示"） */

bool p25rls_pic_save(uint8_t slot, const uint8_t *bitmap)
{
    s_pic_save_cnt++;
    s_pic_save_slot = slot;
    if (bitmap != nullptr) memcpy(s_pic_save_bitmap, bitmap, P25RLS_BITMAP_BYTES);
    s_order = 1;
    return true;
}

void p25rls_pic_show(uint8_t slot)
{
    s_pic_show_cnt++;
    s_pic_show_slot = slot;
    if (s_order == 1) s_order = 2;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static app_ccb_t s_ccb = {.name = "p25rls_ut", .ops = nullptr};

/** @brief 清空所有记录与几何（每个用例开头调一次） */
static void fixture_reset(void)
{
    s_tx_cnt = 0;
    s_tx_len = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));

    s_draw_cnt = 0;
    memset(s_draws, 0, sizeof(s_draws));

    s_fixed_cnt  = 0;
    s_fixed_last = 0xFF;
    s_resume_cnt = 0;

    static const uint8_t k_ip[4]   = {10, 20, 30, 40};
    static const uint8_t k_mask[4] = {255, 255, 255, 0};
    static const uint8_t k_gw[4]   = {10, 20, 30, 1};
    memcpy(s_run_ip, k_ip, 4);
    memcpy(s_run_mask, k_mask, 4);
    memcpy(s_run_gw, k_gw, 4);

    memset(&s_iap_rec, 0, sizeof(s_iap_rec));
    s_iap_rec_valid = false;
    memset(s_iap_new_ip, 0, 4);
    memset(s_iap_new_mask, 0, 4);
    memset(s_iap_new_gw, 0, 4);
    s_iap_new_port = 0xDEADBEEF;
    s_iap_upd_cnt  = 0;
    s_tcp_port     = 10011;

    s_pic_save_cnt = 0;
    s_pic_save_slot = 0xFF;
    memset(s_pic_save_bitmap, 0, sizeof(s_pic_save_bitmap));
    s_pic_show_cnt  = 0;
    s_pic_show_slot = 0xFF;
    s_order         = 0;

    s_rows = 128;
    s_cols = 32;
}

/* ---- 帧构造 ---- */
static uint8_t s_frame[P25RLS_FRAME_MAX_BYTES];

/** @brief 按字段构造一整帧（长度域 = 整帧字节数，大端；BCC 按"帧头 → BCC 前"异或）
 *  @return 整帧字节数 */
static uint16_t frame_build(app_p25rls_cmd_type_t cmd, const uint8_t *data, uint16_t data_len)
{
    const uint16_t total  = (uint16_t)(sizeof(app_p25rls_frame_t) + data_len + 3U);
    app_p25rls_frame_t *f = (app_p25rls_frame_t *)s_frame;

    memset(s_frame, 0, sizeof(s_frame));
    f->head[0]   = P25RLS_HEAD_BYTE0;
    f->head[1]   = P25RLS_HEAD_BYTE1;
    f->length[0] = (uint8_t)(total >> 8);
    f->length[1] = (uint8_t)(total & 0xFFU);
    f->cmd[0]    = (uint8_t)((uint16_t)cmd & 0xFFU);
    f->cmd[1]    = (uint8_t)(((uint16_t)cmd >> 8) & 0xFFU);

    if ((data != nullptr) && (data_len > 0)) memcpy(f->data_bcc_tail, data, data_len);

    s_frame[total - 3U] = bcc_calc(s_frame, (uint16_t)(total - 3U));
    s_frame[total - 2U] = P25RLS_TAIL_BYTE0;
    s_frame[total - 1U] = P25RLS_TAIL_BYTE1;
    return total;
}

/** @brief 独立的 BCC oracle（逐字节异或，不调被测实现） */
static uint8_t bcc_oracle(const uint8_t *data, uint16_t len)
{
    uint8_t acc = 0;
    for (uint16_t i = 0; i < len; i++) acc ^= data[i];
    return acc;
}

/* ---- 载荷构造 ---- */
static uint8_t s_payload[P25RLS_BITMAP_BYTES + 16];

/** @brief 构造 SV 的 DATA（亮度 + 图片号 + 576B 位图），位图按固定花样填
 *  @param brightness 线上亮度
 *  @param slot 图片编号
 *  @return DATA 字节数 */
static uint16_t display_payload(uint8_t brightness, uint8_t slot)
{
    memset(s_payload, 0, P25RLS_DISPLAY_PAYLOAD_BYTES);
    s_payload[0] = brightness;
    s_payload[1] = slot;
    for (uint16_t i = 0; i < P25RLS_BITMAP_BYTES; i++)
        s_payload[2 + i] = (uint8_t)(i * 7U + 3U);
    return (uint16_t)P25RLS_DISPLAY_PAYLOAD_BYTES;
}

/** @brief 构造 HZ 的 DATA（颜色 + 点阵大小 + GBK 文本）
 *  @return DATA 字节数 */
static uint16_t text_payload(uint8_t color, uint8_t dot_size, const char *text, uint16_t text_len)
{
    memset(s_payload, 0, sizeof(app_p25rls_text_t) + text_len);
    s_payload[0] = color;
    s_payload[1] = dot_size;
    if ((text != nullptr) && (text_len > 0))
        memcpy(&s_payload[sizeof(app_p25rls_text_t)], text, text_len);
    return (uint16_t)(sizeof(app_p25rls_text_t) + text_len);
}

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 命令表：7 条主机→设备命令逐条可达，未知命令字静默丢弃
 *
 *  P25 的命令字是 ASCII 两字节按小端拼的 uint16（`"SV"` → 0x5653）。若按大端拼，
 *  表里 7 条全部对不上，现场就是"设备收到帧但一条命令都不执行" —— 这只能靠本用例
 *  把"每个命令字都能落地"钉住。 */
static void case_dispatch(void)
{
    TEST_BEGIN("命令分派：7 条命令逐条可达，未知命令字丢弃");

    /* 每条命令挑一个可观察的副作用，确认**真的分派到了**对应的处理函数 */
    struct {
        app_p25rls_cmd_type_t cmd;
        const char           *name;
    } const table[] = {
        {APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, "SV"},
        {APP_P25RLS_CMD_TYPE_SET_IP, "IP"},
        {APP_P25RLS_CMD_TYPE_AQUIRY_IP, "SC"},
        {APP_P25RLS_CMD_TYPE_FILL, "FS"},
        {APP_P25RLS_CMD_TYPE_TEXT, "HZ"},
        {APP_P25RLS_CMD_TYPE_CLEAR, "CL"},
        {APP_P25RLS_CMD_TYPE_BAUD, "BD"},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        fixture_reset();

        /* 给每条命令一个"最小合法载荷"，只关心它有没有被处理（副作用见各自用例） */
        uint16_t data_len = 0;
        switch (table[i].cmd) {
            case APP_P25RLS_CMD_TYPE_DISPLAY_SAVE: data_len = display_payload(1, 0); break;
            case APP_P25RLS_CMD_TYPE_SET_IP:
                memset(s_payload, 1, P25RLS_NET_CFG_BYTES);
                data_len = P25RLS_NET_CFG_BYTES;
                break;
            case APP_P25RLS_CMD_TYPE_FILL: s_payload[0] = 1; data_len = 1; break;
            case APP_P25RLS_CMD_TYPE_TEXT: data_len = text_payload(1, 0, "A", 1); break;
            case APP_P25RLS_CMD_TYPE_CLEAR: data_len = 0; break;
            case APP_P25RLS_CMD_TYPE_BAUD: s_payload[0] = 0x02; data_len = 1; break;
            default: data_len = 0; break; /* SC 的 DATA 不参与应答 */
        }

        const uint16_t n = frame_build(table[i].cmd, s_payload, data_len);
        app_p25rls_handle_frame(&s_ccb, s_frame, n);

        switch (table[i].cmd) {
            case APP_P25RLS_CMD_TYPE_DISPLAY_SAVE:
                CHECK_MSG(s_pic_save_cnt == 1, "%s：应存槽 1 次，实际 %d", table[i].name,
                          s_pic_save_cnt);
                break;
            case APP_P25RLS_CMD_TYPE_SET_IP:
                CHECK_MSG(s_iap_upd_cnt == 1, "%s：应写记录 1 次，实际 %d", table[i].name,
                          s_iap_upd_cnt);
                break;
            case APP_P25RLS_CMD_TYPE_AQUIRY_IP:
                CHECK_MSG(s_tx_cnt == 1, "%s：应回 1 帧，实际 %d", table[i].name, s_tx_cnt);
                break;
            case APP_P25RLS_CMD_TYPE_FILL:
            case APP_P25RLS_CMD_TYPE_CLEAR:
                CHECK_MSG(s_draw_cnt == 1, "%s：应画 1 笔（整屏填充），实际 %d", table[i].name,
                          s_draw_cnt);
                break;
            case APP_P25RLS_CMD_TYPE_TEXT:
                CHECK_MSG(s_draw_cnt == 2, "%s：应清屏 + 文字 2 笔，实际 %d", table[i].name,
                          s_draw_cnt);
                break;
            case APP_P25RLS_CMD_TYPE_BAUD:
                /* 只记日志：不画、不发、不存 */
                CHECK_MSG((s_draw_cnt == 0) && (s_tx_cnt == 0) && (s_pic_save_cnt == 0),
                          "%s：只记日志，实际 draw=%d tx=%d save=%d", table[i].name, s_draw_cnt,
                          s_tx_cnt, s_pic_save_cnt);
                break;
            default: break;
        }
    }

    /* 未知命令字：不画、不发、不存 */
    fixture_reset();
    const uint16_t n = frame_build((app_p25rls_cmd_type_t)0x4242, nullptr, 0);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_draw_cnt == 0) && (s_tx_cnt == 0) && (s_pic_save_cnt == 0),
              "未知命令字应丢弃，实际 draw=%d tx=%d save=%d", s_draw_cnt, s_tx_cnt,
              s_pic_save_cnt);
}

/** @brief SV：**先存槽、再显示**；亮度 0~31 → 面板 1~7；位图 576B 原样进槽
 *
 *  顺序反了会让"存失败"把屏清掉；亮度映射错就是现场"屏太暗"；位图错位是整屏花。 */
static void case_display_save(void)
{
    TEST_BEGIN("SV：亮度映射 + 图片号进槽 + 先存后显示 + 576B 位图原样");

    fixture_reset();
    const uint16_t data_len = display_payload(20, 1);
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, s_payload, data_len);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_fixed_cnt == 1, "应设 1 次固定亮度，实际 %d", s_fixed_cnt);
    CHECK_MSG(s_fixed_last == 5, "亮度 20 → 面板 1+((20*6+15)/31) = 5，实际 %u",
              (unsigned)s_fixed_last);
    CHECK_MSG(s_resume_cnt == 0, "非 255 不该恢复自动亮度");

    CHECK_MSG(s_pic_save_cnt == 1, "应存槽 1 次，实际 %d", s_pic_save_cnt);
    CHECK_MSG(s_pic_save_slot == 1, "图片编号 1 → 槽 1，实际 %u", (unsigned)s_pic_save_slot);
    CHECK_MSG(0 == memcmp(s_pic_save_bitmap, &s_payload[2], P25RLS_BITMAP_BYTES),
              "进槽的 576B 位图应与载荷逐字节相同");

    CHECK_MSG(s_pic_show_cnt == 1, "应显示 1 次，实际 %d", s_pic_show_cnt);
    CHECK_MSG(s_pic_show_slot == 1, "应显示槽 1，实际 %u", (unsigned)s_pic_show_slot);
    CHECK_MSG(s_order == 2, "必须先 save 后 show（实际顺序码 %d）", s_order);

    CHECK_MSG(s_draw_cnt == 0, "SV 的绘制在图片槽模块里，命令层不该直接画（实际 %d 笔）",
              s_draw_cnt);
    CHECK_MSG(s_tx_cnt == 0, "SV 无应答，实际发了 %d 帧", s_tx_cnt);
}

/** @brief SV 的亮度边界：0 → 最暗 1、31 → 最亮 7、越界 40 夹到 31、255 = 恢复自动 */
static void case_display_light(void)
{
    TEST_BEGIN("SV 亮度：0→1 / 31→7 / 40 夹到 7 / 16→4 / 255 → 恢复自动");

    struct {
        uint8_t in;
        uint8_t want;
    } const cases[] = {{0, 1}, {31, 7}, {40, 7}, {16, 4}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fixture_reset();
        const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, s_payload,
                                       display_payload(cases[i].in, 0));
        app_p25rls_handle_frame(&s_ccb, s_frame, n);

        CHECK_MSG(s_fixed_last == cases[i].want, "亮度 %u 应映射到 %u，实际 %u",
                  (unsigned)cases[i].in, (unsigned)cases[i].want, (unsigned)s_fixed_last);
    }

    fixture_reset();
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, s_payload,
                                   display_payload(255, 0));
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_resume_cnt == 1, "亮度 255 应恢复自动（调 resume 1 次，实际 %d）", s_resume_cnt);
    CHECK_MSG(s_fixed_cnt == 0, "亮度 255 不该同时设固定值，实际 %d", s_fixed_cnt);
}

/** @brief SV 的载荷门禁：短帧 / 图片编号越界都**整帧丢弃**，不动亮度、不存、不显示 */
static void case_display_guard(void)
{
    TEST_BEGIN("SV 门禁：短帧丢弃 / 图片编号 ≥3 丢弃（不动屏、不动亮度）");

    fixture_reset();
    uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, s_payload,
                             (uint16_t)(P25RLS_DISPLAY_PAYLOAD_BYTES - 1U));
    memset(s_payload, 0xAA, P25RLS_BITMAP_BYTES);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_pic_save_cnt == 0) && (s_pic_show_cnt == 0) && (s_fixed_cnt == 0),
              "短帧应整帧丢弃（save=%d show=%d fixed=%d）", s_pic_save_cnt, s_pic_show_cnt,
              s_fixed_cnt);

    fixture_reset();
    n = frame_build(APP_P25RLS_CMD_TYPE_DISPLAY_SAVE, s_payload, display_payload(10, 3));
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_pic_save_cnt == 0) && (s_pic_show_cnt == 0) && (s_fixed_cnt == 0),
              "图片编号 3 应整帧丢弃（save=%d show=%d fixed=%d）", s_pic_save_cnt, s_pic_show_cnt,
              s_fixed_cnt);
}

/** @brief FS 全屏纯色：1/2/3 → 红/绿/黄，铺满逻辑屏且**不落盘**；0/4 丢弃 */
static void case_fill(void)
{
    TEST_BEGIN("FS 全屏纯色：1/2/3 → 红/绿/黄，铺满逻辑屏、不落盘；越界丢弃");

    struct {
        uint8_t in;
        uint8_t want;
    } const cases[] = {{1, DEV_DISPLAY_COLOR_RED},
                       {2, DEV_DISPLAY_COLOR_GREEN},
                       {3, DEV_DISPLAY_COLOR_YELLOW}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fixture_reset();
        s_payload[0]     = cases[i].in;
        const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_FILL, s_payload, 1);
        app_p25rls_handle_frame(&s_ccb, s_frame, n);

        CHECK_MSG(s_draw_cnt == 1, "颜色 %u：应画 1 笔，实际 %d", (unsigned)cases[i].in,
                  s_draw_cnt);
        if (s_draw_cnt == 1) {
            CHECK_MSG(s_draws[0].type == APP_RENDER_TYPE_FILL, "应是整屏填充");
            CHECK_MSG(s_draws[0].color == cases[i].want, "颜色 %u → %u，实际 %u",
                      (unsigned)cases[i].in, (unsigned)cases[i].want, (unsigned)s_draws[0].color);
            CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "应铺满逻辑屏，实际 %ux%u",
                      (unsigned)s_draws[0].w, (unsigned)s_draws[0].h);
            CHECK_MSG(!s_draws[0].persist, "FS 标注'测试不保存'，不得落盘");
        }
        CHECK_MSG(s_tx_cnt == 0, "FS 无应答");
    }

    /* 颜色 0（黑）/ 4（越界）/ 255：整帧丢弃，不能透传给扫描驱动 */
    const uint8_t bad[] = {0, 4, 0xFF};
    for (size_t i = 0; i < sizeof(bad); i++) {
        fixture_reset();
        s_payload[0]     = bad[i];
        const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_FILL, s_payload, 1);
        app_p25rls_handle_frame(&s_ccb, s_frame, n);
        CHECK_MSG(s_draw_cnt == 0, "颜色码 %u 应整帧丢弃，实际画了 %d 笔", (unsigned)bad[i],
                  s_draw_cnt);
    }

    /* 空载荷 */
    fixture_reset();
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_FILL, nullptr, 0);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "空载荷应整帧丢弃");
}

/** @brief HZ 文字：先清整屏、再按"无坐标/无字型"口径顶格排；颜色与点阵大小逐档映射
 *
 *  协议没有坐标与字型字段 → 固定从 (0,0)、宋体、GBK 直通；区域取整个逻辑屏。
 *  先清屏的理由：换一条更短的字，不清就会新旧叠在一起。 */
static void case_text(void)
{
    TEST_BEGIN("HZ 文字：先清屏 + 顶格宋体 GBK + 颜色/点阵大小映射");

    fixture_reset();
    const uint16_t n =
        frame_build(APP_P25RLS_CMD_TYPE_TEXT, s_payload, text_payload(2, 1, "AB", 2));
    app_p25rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_draw_cnt == 2, "应是清屏 + 文字两笔，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].type == APP_RENDER_TYPE_FILL, "第一笔应是清屏");
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏应为黑，实际 %u",
              (unsigned)s_draws[0].color);
    CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "清屏应铺满逻辑屏，实际 %ux%u",
              (unsigned)s_draws[0].w, (unsigned)s_draws[0].h);
    CHECK_MSG(!s_draws[0].persist, "清屏不得落盘");

    const draw_t *t = &s_draws[1];
    CHECK_MSG(t->type == APP_RENDER_TYPE_TEXT, "第二笔应是文字");
    CHECK_MSG((t->x == 0) && (t->y == 0), "无坐标字段 → 从 (0,0) 起排，实际 (%u,%u)",
              (unsigned)t->x, (unsigned)t->y);
    CHECK_MSG((t->w == 128) && (t->h == 32), "区域应取整个逻辑屏，实际 %ux%u", (unsigned)t->w,
              (unsigned)t->h);
    CHECK_MSG(t->color == DEV_DISPLAY_COLOR_GREEN, "颜色 2 → 绿，实际 %u", (unsigned)t->color);
    CHECK_MSG(t->font_size == APP_FONT_SIZE_24, "点阵大小 1 → 24，实际 %u", (unsigned)t->font_size);
    CHECK_MSG(t->font_type == APP_FONT_TYPE_ST, "协议无字型字段 → 宋体，实际 %u",
              (unsigned)t->font_type);
    CHECK_MSG(t->text_enc == APP_FONT_ENC_GBK, "文本按 GBK 直通，实际 %u", (unsigned)t->text_enc);
    CHECK_MSG((t->text_len == 2) && (0 == memcmp(t->text, "AB", 2)), "文本长度/内容应原样下传");
    CHECK_MSG((t->h_align == APP_RENDER_ALIGN_LEFT_UP) && (t->v_align == APP_RENDER_ALIGN_LEFT_UP),
              "顶格左对齐（协议没有对齐字段）");
    CHECK_MSG(!t->persist, "文字不落盘（文档只承诺 SV 断电保存）");
    CHECK_MSG(s_tx_cnt == 0, "HZ 无应答，实际发了 %d 帧", s_tx_cnt);
}

/** @brief HZ 的数值域：点阵大小 0/1/2 → 16/24/32、越界回落 16；颜色 1/3 与越界 */
static void case_text_map(void)
{
    TEST_BEGIN("HZ 映射：点阵 0/1/2 → 16/24/32，越界回落 16；颜色 1/3 合法、其余丢弃");

    struct {
        uint8_t in;
        uint8_t want;
    } const sizes[] = {{0, APP_FONT_SIZE_16}, {1, APP_FONT_SIZE_24}, {2, APP_FONT_SIZE_32},
                       {3, APP_FONT_SIZE_16}, {0xFF, APP_FONT_SIZE_16}};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fixture_reset();
        const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_TEXT, s_payload,
                                       text_payload(1, sizes[i].in, "A", 1));
        app_p25rls_handle_frame(&s_ccb, s_frame, n);
        CHECK_MSG((s_draw_cnt == 2) && (s_draws[1].font_size == sizes[i].want),
                  "点阵 %u 应映射到 %u，实际 %u", (unsigned)sizes[i].in, (unsigned)sizes[i].want,
                  (unsigned)(s_draw_cnt == 2 ? s_draws[1].font_size : 0xFF));
    }

    const uint8_t colors[] = {1, 3, 0, 4, 0xFF};
    const uint8_t want[]   = {DEV_DISPLAY_COLOR_RED, DEV_DISPLAY_COLOR_YELLOW, 0, 0, 0};
    for (size_t i = 0; i < sizeof(colors); i++) {
        fixture_reset();
        const uint16_t n =
            frame_build(APP_P25RLS_CMD_TYPE_TEXT, s_payload, text_payload(colors[i], 0, "A", 1));
        app_p25rls_handle_frame(&s_ccb, s_frame, n);
        if (want[i] != 0) {
            CHECK_MSG((s_draw_cnt == 2) && (s_draws[1].color == want[i]),
                      "颜色 %u 应映射到 %u，实际 %u", (unsigned)colors[i], (unsigned)want[i],
                      (unsigned)(s_draw_cnt == 2 ? s_draws[1].color : 0xFF));
        } else {
            CHECK_MSG(s_draw_cnt == 0, "颜色 %u 应整帧丢弃，实际画了 %d 笔", (unsigned)colors[i],
                      s_draw_cnt);
        }
    }

    /* 载荷连固定头都不够 */
    fixture_reset();
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_TEXT, s_payload, 2);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "只有固定头、没有文本 → 整帧丢弃，实际画了 %d 笔", s_draw_cnt);
}

/** @brief CL 清屏：整屏填黑、不落盘、无应答 */
static void case_clear(void)
{
    TEST_BEGIN("CL 清屏：整屏黑 + 不落盘");

    fixture_reset();
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_CLEAR, nullptr, 0);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_draw_cnt == 1, "应画 1 笔，实际 %d", s_draw_cnt);
    CHECK_MSG(s_draws[0].type == APP_RENDER_TYPE_FILL, "应是整屏填充");
    CHECK_MSG(s_draws[0].color == DEV_DISPLAY_COLOR_BLACK, "清屏应为黑，实际 %u",
              (unsigned)s_draws[0].color);
    CHECK_MSG((s_draws[0].w == 128) && (s_draws[0].h == 32), "应铺满逻辑屏，实际 %ux%u",
              (unsigned)s_draws[0].w, (unsigned)s_draws[0].h);
    CHECK_MSG(!s_draws[0].persist, "清屏不落盘（上电仍恢复最后一次 SV）");
    CHECK_MSG(s_tx_cnt == 0, "CL 无应答");
}

/** @brief IP 改地址：**只写记录、不重启、不回包**；端口沿用记录里的值
 *
 *  协议帧里没有端口字段。若这里把 port 写成 0（或运行态端口），设备重启后监听的是
 *  另一个端口 —— 现场表现就是"改了 IP 就再也连不上"。 */
static void case_set_ip(void)
{
    TEST_BEGIN("IP 改地址：写记录、不回包、端口沿用记录值（无记录时用运行态端口）");

    fixture_reset();
    s_iap_rec_valid = true;
    s_iap_rec.port  = 12345;
    const uint8_t data[P25RLS_NET_CFG_BYTES] = {192, 168, 8, 100, 255, 255, 255, 0, 192, 168, 8, 1};
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_SET_IP, data, P25RLS_NET_CFG_BYTES);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_iap_upd_cnt == 1, "应写记录 1 次，实际 %d", s_iap_upd_cnt);
    CHECK_MSG(0 == memcmp(s_iap_new_ip, data, 4), "IP 应原样写入");
    CHECK_MSG(0 == memcmp(s_iap_new_mask, &data[4], 4), "掩码应原样写入");
    CHECK_MSG(0 == memcmp(s_iap_new_gw, &data[8], 4), "网关应原样写入");
    CHECK_MSG(s_iap_new_port == 12345, "端口应沿用记录里的 12345，实际 %u",
              (unsigned)s_iap_new_port);
    CHECK_MSG(s_tx_cnt == 0, "IP 命令协议未定义应答，实际发了 %d 帧", s_tx_cnt);
    CHECK_MSG(s_draw_cnt == 0, "改地址不该动屏");

    /* 记录无效（首次上电/记录损坏）：端口回落运行态 */
    fixture_reset();
    s_iap_rec_valid   = false;
    s_tcp_port        = 10011;
    const uint16_t n2 = frame_build(APP_P25RLS_CMD_TYPE_SET_IP, data, P25RLS_NET_CFG_BYTES);
    app_p25rls_handle_frame(&s_ccb, s_frame, n2);
    CHECK_MSG(s_iap_upd_cnt == 1, "无记录时也要写记录");
    CHECK_MSG(s_iap_new_port == 10011, "无记录时端口回落运行态 10011，实际 %u",
              (unsigned)s_iap_new_port);

    /* 短帧：11 字节，字段不全 → 丢弃 */
    fixture_reset();
    const uint16_t n3 = frame_build(APP_P25RLS_CMD_TYPE_SET_IP, data, P25RLS_NET_CFG_BYTES - 1U);
    app_p25rls_handle_frame(&s_ccb, s_frame, n3);
    CHECK_MSG(s_iap_upd_cnt == 0, "短帧应整帧丢弃，实际写了 %d 次", s_iap_upd_cnt);
}

/** @brief SC 查询 IP：应答帧 `FF FE 00 15 'R' 'B' + IP + 掩码 + 网关 + BCC + 0D 0C`
 *
 *  这是本协议唯一的输出帧，逐字节都是线上契约：长度域大端 21、命令字是应答的 ASCII
 *  字符序、BCC 覆盖前 18 字节、帧尾 0D 0C。回的是**运行态**地址（不是记录里的旧值）。 */
static void case_aquire_ip(void)
{
    TEST_BEGIN("SC 查询 IP：21 字节 RB 应答逐字节（运行态地址 + BCC）");

    fixture_reset();
    s_iap_rec_valid = true;
    const uint8_t rec_ip[4] = {1, 1, 1, 1};
    memcpy(s_iap_rec.ip, rec_ip, 4); /* 记录里的旧值：不得出现在应答里 */

    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_AQUIRY_IP, nullptr, 0);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_tx_cnt == 1, "应回 1 帧，实际 %d", s_tx_cnt);
    CHECK_MSG(s_tx_len == P25RLS_REPORT_IP_BYTES, "应答应为 %u 字节，实际 %u",
              (unsigned)P25RLS_REPORT_IP_BYTES, (unsigned)s_tx_len);
    if (s_tx_len != P25RLS_REPORT_IP_BYTES) return;

    CHECK_MSG(s_tx_buf[0] == 0xFF && s_tx_buf[1] == 0xFE, "帧头应 FF FE，实际 %02X %02X",
              (unsigned)s_tx_buf[0], (unsigned)s_tx_buf[1]);
    CHECK_MSG(s_tx_buf[2] == 0x00 && s_tx_buf[3] == 0x15, "长度域应大端 00 15，实际 %02X %02X",
              (unsigned)s_tx_buf[2], (unsigned)s_tx_buf[3]);
    CHECK_MSG(s_tx_buf[4] == 'R' && s_tx_buf[5] == 'B', "命令字应是 ASCII 'R' 'B'（应答），实际 %02X %02X",
              (unsigned)s_tx_buf[4], (unsigned)s_tx_buf[5]);
    CHECK_MSG(0 == memcmp(&s_tx_buf[6], s_run_ip, 4), "应答应带运行态 IP，实际 %u.%u.%u.%u",
              (unsigned)s_tx_buf[6], (unsigned)s_tx_buf[7], (unsigned)s_tx_buf[8],
              (unsigned)s_tx_buf[9]);
    CHECK_MSG(0 == memcmp(&s_tx_buf[10], s_run_mask, 4), "应答应带运行态掩码");
    CHECK_MSG(0 == memcmp(&s_tx_buf[14], s_run_gw, 4), "应答应带运行态网关");
    CHECK_MSG(s_tx_buf[18] == bcc_oracle(s_tx_buf, 18),
              "BCC 应覆盖前 18 字节（得 %02X 期望 %02X）", (unsigned)s_tx_buf[18],
              (unsigned)bcc_oracle(s_tx_buf, 18));
    CHECK_MSG(s_tx_buf[19] == 0x0D && s_tx_buf[20] == 0x0C, "帧尾应 0D 0C，实际 %02X %02X",
              (unsigned)s_tx_buf[19], (unsigned)s_tx_buf[20]);
    CHECK_MSG(s_draw_cnt == 0, "查询不该动屏");
}

/** @brief BD 改波特率：本协议走 UDP，**只接受不动作**（不画、不发、不存、不崩） */
static void case_baud(void)
{
    TEST_BEGIN("BD 波特率：只记录、不动作（空载荷也不崩）");

    fixture_reset();
    s_payload[0]     = 0x02;
    const uint16_t n = frame_build(APP_P25RLS_CMD_TYPE_BAUD, s_payload, 1);
    app_p25rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG((s_draw_cnt == 0) && (s_tx_cnt == 0) && (s_pic_save_cnt == 0),
              "BD 不该有副作用（draw=%d tx=%d save=%d）", s_draw_cnt, s_tx_cnt, s_pic_save_cnt);

    fixture_reset();
    const uint16_t n2 = frame_build(APP_P25RLS_CMD_TYPE_BAUD, nullptr, 0);
    app_p25rls_handle_frame(&s_ccb, s_frame, n2);
    CHECK_MSG((s_draw_cnt == 0) && (s_tx_cnt == 0), "空载荷 BD 不该有副作用");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[1mP25RLS 命令层\033[0m\n");

    case_dispatch();
    case_display_save();
    case_display_light();
    case_display_guard();
    case_fill();
    case_text();
    case_text_map();
    case_clear();
    case_set_ip();
    case_aquire_ip();
    case_baud();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
