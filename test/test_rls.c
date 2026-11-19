/**
 * @file    test_rls.c
 * @brief   RLS（四川车道可变信息屏协议）host 单测：命令分派矩阵 / 回显规则 / 各命令行为
 *          （显示位图门禁与颜色、亮度换算、显示开关、修改 IP、搜索应答、图片槽 SV/DS）
 *
 * 帧探测（帧头 / 长度域 / 帧尾 / 暂存区契约）在 `test_probes.c` 里测，本套件不重复；
 * 这里从"一帧完整帧"往里测：调 `app_rls_handle_frame()`，看**真命令表**分派到哪个
 * 处理函数、回了什么、对渲染 / 图片槽 / 屏开关 / 网络做了什么。
 *
 * **为什么需要这个测试**：
 *   1. 命令码 → 处理函数是**两张按位置对应的表**（`app_rls.c` 的 `cmd_index_table` 与
 *      `app_rls_cmd.c` 的 `g_rls_cmd_table`）。错一位就是"命令 A 干了 B 的事"，错到底
 *      就是越界调用；两张表在编译期没有交叉校验，只能逐命令喂帧来钉。
 *   2. 回显规则是现场与上位机的约定：发图两条（BM/SV）与查询类（ST/SC）不回显，其余
 *      回**原帧**。错了表现为"上位机等不到应答"或"一个请求两帧"。
 *   3. 三条"数值域"守卫各自对应一类越界：颜色 >7 会越界查扫描的 BSRR 表、位图不足会
 *      让 `app_render` 越界读帧缓冲、图片编号越界会写错槽。
 *
 * 手法：生产 TU（`app_rls.c` / `app_rls_cmd.c`）**照原样编译**（不 TU-include），
 * 副作用用记录桩观察（渲染 / 光感 / IAP / 网络 / 通道发送 / 图片槽）；BCC 用真实实现。
 * 帧由用例独立构造，按**帧结构体的字段布局**写入 —— 结构体即线格式契约。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "app_rls.h"
#include "app_rls_cmd.h"
#include "app_rls_pic.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "bcc_utils.h"
#include "pl_net.h"

/* ================================================================
 *  断言（与其它套件同源变体，见 docs/测试约定.md 附录 A）
 * ================================================================ */

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

/* ================================================================
 *  记录桩：命令处理函数的所有外部副作用
 * ================================================================ */

/* ---- 通道发送（回显 / 搜索应答都走它）---- */
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

/* ---- 渲染：只记几何 / 颜色 / persist ---- */
typedef struct {
    bool     bitmap;
    uint16_t w;
    uint16_t h;
    uint8_t  color;
    bool     persist;
} draw_t;

#define DRAW_MAX (8U)
static draw_t s_draws[DRAW_MAX];
static int    s_draw_cnt;

void app_render(const app_render_cfg_t *cfg)
{
    if (s_draw_cnt >= (int)DRAW_MAX) return;

    draw_t *d    = &s_draws[s_draw_cnt++];
    d->bitmap    = (cfg->type == APP_RENDER_TYPE_BITMAP);
    d->w         = cfg->w;
    d->h         = cfg->h;
    d->color     = (uint8_t)cfg->color;
    d->persist   = cfg->persist;
}

/* ---- 逻辑屏几何（3833024：128×32，正好 512B 位图）---- */
static uint16_t s_rows = 128;
static uint16_t s_cols = 32;

uint16_t app_screen_rows(void) { return s_rows; }
uint16_t app_screen_cols(void) { return s_cols; }

/* ---- 屏开关（只切 OE）---- */
static int s_oe_on;
static int s_oe_off;

void app_screen_oe_enable(void) { s_oe_on++; }
void app_screen_oe_disable(void) { s_oe_off++; }

/* ---- 亮度：固定档 / 恢复自动跟随 ---- */
static int     s_fixed_cnt;
static uint8_t s_fixed_last;
static int     s_resume_cnt;

void app_light_sensor_set_fixed(uint8_t level)
{
    s_fixed_cnt++;
    s_fixed_last = level;
}
void app_light_sensor_resume(void) { s_resume_cnt++; }

/* ---- 网络 / IAP 记录 ---- */
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

/* ---- 图片槽 ---- */
static int     s_pic_save_cnt;
static uint8_t s_pic_save_slot;
static uint8_t s_pic_save_color;
static int     s_pic_show_cnt;
static uint8_t s_pic_show_slot;

bool rls_pic_save(uint8_t slot, const uint8_t *bitmap, uint8_t color)
{
    (void)bitmap;
    s_pic_save_cnt++;
    s_pic_save_slot  = slot;
    s_pic_save_color = color;
    return true;
}

void rls_pic_show(uint8_t slot)
{
    s_pic_show_cnt++;
    s_pic_show_slot = slot;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static app_ccb_t s_ccb = {.name = "rls_ut", .ops = nullptr};

/** @brief 清空所有记录（每个用例开头调一次） */
static void fixture_reset(void)
{
    s_tx_cnt = 0;
    s_tx_len = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));

    s_draw_cnt = 0;
    memset(s_draws, 0, sizeof(s_draws));

    s_oe_on = s_oe_off = 0;
    s_fixed_cnt        = 0;
    s_fixed_last       = 0;
    s_resume_cnt       = 0;

    s_iap_upd_cnt = 0;
    memset(s_iap_new_ip, 0, 4);
    memset(s_iap_new_mask, 0, 4);
    memset(s_iap_new_gw, 0, 4);
    s_iap_new_port = 0;

    s_pic_save_cnt = 0;
    s_pic_save_slot = 0;
    s_pic_save_color = 0;
    s_pic_show_cnt = 0;
    s_pic_show_slot = 0;

    s_rows = 128;
    s_cols = 32;
}

/** @brief 帧缓冲：head + len(2, 大端) + cmd(2) + payload + BCC + tail
 *
 *  返回**整帧字节数**；BCC 填 0（本协议接收端不校验）。payload 里的命令码按小端写，
 *  与接收端"两字节拼 uint16"的读法一致。 */
static uint8_t s_frame[600];

static uint16_t frame_build(app_rls_cmd_type_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    const uint16_t total = (uint16_t)(sizeof(app_rls_frame_t) + payload_len + 3U);

    memset(s_frame, 0, sizeof(s_frame));
    s_frame[0] = 0xFF;
    s_frame[1] = 0xFE;
    s_frame[2] = (uint8_t)(total >> 8);
    s_frame[3] = (uint8_t)(total & 0xFFU);
    s_frame[4] = (uint8_t)((uint16_t)cmd & 0xFFU);
    s_frame[5] = (uint8_t)(((uint16_t)cmd >> 8) & 0xFFU);

    if (payload != nullptr && payload_len > 0) memcpy(&s_frame[6], payload, payload_len);

    s_frame[total - 3] = 0x00; /* BCC */
    s_frame[total - 2] = 0x0D;
    s_frame[total - 1] = 0x0C;
    return total;
}

/** @brief 构造显示载荷：亮度 + 颜色 + 风格 + 图片编号 + n 字节位图 */
static uint8_t  s_disp[600];
static uint16_t disp_payload(uint8_t light, uint8_t color, uint8_t pic, uint16_t bitmap_len)
{
    s_disp[0] = light;
    s_disp[1] = color;
    s_disp[2] = 0; /* style */
    s_disp[3] = pic;
    memset(&s_disp[4], 0x55, bitmap_len); /* 位图内容不参与断言，填个非零图案 */
    return (uint16_t)(sizeof(app_rls_display_t) + bitmap_len);
}

/** @brief 位图字节数（128×32 → 512），显示类命令的"整帧恰好 525B"就是它 */
#define DISP_FULL_FRAME (13U + 512U)

/* ================================================================
 *  用例 1：分派矩阵 —— 每个线上命令码都必须落到自己的处理函数
 * ================================================================ */

/**
 * @brief 分派矩阵：7 类命令码各自落到对应处理函数，未知码什么都不做
 *
 * 两张表按位置对应，错位时这里的"效果断言"会指向别的处理函数：例如 SV 的码若查到
 * BM 的处理函数，就会看到 2 次渲染而不是存槽；未知码若误中某一项，就会看到不该有的
 * 副作用。
 */
static void case_dispatch_matrix(void)
{
    TEST_BEGIN("分派矩阵：BM/SV/DS/55/IP/ST/SC 各归其位，未知码无副作用");
    fixture_reset();

    /* BM：清屏 + 贴图 */
    uint16_t n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(0, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(n == DISP_FULL_FRAME && s_draw_cnt == 2, "BM 应清屏+贴图（2 次绘制），得到 %d 次",
              s_draw_cnt);

    /* SV：存槽 + 显示槽（不直接渲染） */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 1, 2, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_save_cnt == 1 && s_pic_save_slot == 2 && s_pic_show_cnt == 1 &&
                  s_draw_cnt == 0,
              "SV 应存槽 2 并显示（save=%d/%u show=%d 绘制=%d）", s_pic_save_cnt,
              (unsigned)s_pic_save_slot, s_pic_show_cnt, s_draw_cnt);

    /* DS：只显示槽 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(0, 1, 1, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_show_cnt == 1 && s_pic_show_slot == 1 && s_draw_cnt == 0,
              "DS 应显示槽 1（show=%d/%u）", s_pic_show_cnt, (unsigned)s_pic_show_slot);

    /* 55：开屏 */
    uint8_t sw = 0x31;
    n         = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_oe_on == 1 && s_oe_off == 0, "55/0x31 应开屏（on=%d off=%d）", s_oe_on, s_oe_off);

    /* IP：写记录 */
    uint8_t net[12] = {10, 0, 0, 5, 255, 255, 255, 0, 10, 0, 0, 1};
    n               = frame_build(APP_RLS_CMD_TYPE_SET_IP, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_iap_upd_cnt == 1 && s_iap_new_ip[3] == 5, "IP 应写 1 次记录（地址尾字节 %u）",
              (unsigned)s_iap_new_ip[3]);

    /* ST / SC：都应发出 RB 应答（21 字节） */
    n = frame_build(APP_RLS_CMD_TYPE_AQUIRY_IP, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == RLS_REPORT_IP_LEN, "ST 应回 21 字节 RB（tx=%d/%u）",
              s_tx_cnt, (unsigned)s_tx_len);

    n = frame_build(APP_RLS_CMD_TYPE_AQUIRY_IP_SC, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == RLS_REPORT_IP_LEN, "SC 也应回 21 字节 RB（tx=%d/%u）",
              s_tx_cnt, (unsigned)s_tx_len);

    /* 未知码：无任何副作用（也不回显） */
    n = frame_build((app_rls_cmd_type_t)0x1234, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 0 && s_draw_cnt == 0 && s_oe_on == 0 && s_iap_upd_cnt == 0,
              "未知码不得有任何副作用（tx=%d 绘制=%d）", s_tx_cnt, s_draw_cnt);
}

/* ================================================================
 *  用例 2：回显规则（线上行为）
 * ================================================================ */

/**
 * @brief 回显规则：BM/SV 与查询类（ST/SC）不回显，其余命令回**原帧**
 *
 * 注意 ST/SC 只回 RB（1 帧），若回显没被排除就会变成 2 帧 —— 这是现场最敏感的一条。
 */
static void case_echo_wire(void)
{
    TEST_BEGIN("回显规则：BM/SV/搜索不回，其余回原帧且字节不变");
    fixture_reset();

    /* BM：0 帧 */
    uint16_t n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(0, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 0, "BM 不得回显（tx=%d）", s_tx_cnt);

    /* SV：0 帧 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 0, "SV 不得回显（tx=%d）", s_tx_cnt);

    /* ST：只有 RB 一帧（21 字节），没有回显 */
    uint8_t  net[12] = {0};
    n                = frame_build(APP_RLS_CMD_TYPE_AQUIRY_IP, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == RLS_REPORT_IP_LEN,
              "ST 只回 RB 一帧（tx=%d len=%u），不得再回显", s_tx_cnt, (unsigned)s_tx_len);

    /* 修改 IP：回的是**原帧**（21 字节，逐字节相同） */
    n = frame_build(APP_RLS_CMD_TYPE_SET_IP, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == n && memcmp(s_tx_buf, s_frame, n) == 0,
              "IP 应原帧回显（tx=%d len=%u，期望 %u 字节逐字节相同）", s_tx_cnt, (unsigned)s_tx_len,
              (unsigned)n);

    /* 开关：原帧回显（10 字节） */
    uint8_t sw = 0x30;
    n          = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == n && memcmp(s_tx_buf, s_frame, n) == 0,
              "开关应原帧回显（tx=%d len=%u）", s_tx_cnt, (unsigned)s_tx_len);

    /* 测试命令：原帧回显（9 字节，无载荷） */
    n = frame_build(APP_RLS_CMD_TYPE_TEST, nullptr, 0);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == 9 && memcmp(s_tx_buf, s_frame, 9) == 0,
              "测试命令应原帧回显 9 字节（tx=%d len=%u）", s_tx_cnt, (unsigned)s_tx_len);

    /* DS：回显（13 字节） */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(0, 1, 0, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == 13, "DS 应原帧回显（tx=%d len=%u）", s_tx_cnt,
              (unsigned)s_tx_len);
}

/* ================================================================
 *  用例 3：显示（BM）
 * ================================================================ */

/**
 * @brief 显示命令：位图不足拒显、颜色同序透传、越界拒显、几何取逻辑屏
 *
 * 位图不足时**一次 app_render 都不许调**（否则渲染器按屏几何越界读帧缓冲）；颜色
 * 与 `dev_display_color_t` 同序透传（旧实现实机口径），>7 整帧拒显。
 */
static void case_display(void)
{
    TEST_BEGIN("显示命令：位图门禁 / 颜色透传与拒显 / 逻辑屏几何");

    /* 位图差 1 字节（524B 帧）→ 拒显 */
    uint16_t n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(10, 1, 0, 511));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "位图 511B（差 1）必须拒显，得到 %d 次绘制", s_draw_cnt);

    /* 恰好 512B → 清屏 + 贴图，几何 = 逻辑屏 128×32，颜色 = 1（红） */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(10, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 2 && !s_draws[0].bitmap && s_draws[1].bitmap,
              "512B 应清屏+贴图（2 次绘制，首笔清屏），得到 %d 次", s_draw_cnt);
    CHECK_MSG(s_draws[1].w == 128 && s_draws[1].h == 32,
              "贴图几何应取逻辑屏 128×32，得到 %ux%u", (unsigned)s_draws[1].w,
              (unsigned)s_draws[1].h);
    CHECK_MSG(s_draws[1].color == 1, "颜色 1 应透传成 1（红），得到 %u", (unsigned)s_draws[1].color);
    CHECK_MSG(s_draws[1].persist == false, "BM（非持久化）不得带 persist");

    /* 颜色 7（白）透传；颜色 8 拒显 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(10, 7, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 2 && s_draws[1].color == 7, "颜色 7 应透传成白，得到 %u",
              (unsigned)s_draws[1].color);

    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(10, 8, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_draw_cnt == 0, "颜色 8 越界必须拒显（否则越界查 BSRR 表），得到 %d 次绘制",
              s_draw_cnt);

    /* 亮度：0 → 面板 1 档；31 → 7 档；255 → 恢复自动跟随 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(0, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_fixed_cnt == 1 && s_fixed_last == 1, "亮度 0 应落到 1 档，得到 %d 档",
              (unsigned)s_fixed_last);

    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(31, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_fixed_cnt == 1 && s_fixed_last == 7, "亮度 31 应落到 7 档，得到 %d 档",
              (unsigned)s_fixed_last);

    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY, s_disp, disp_payload(255, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_resume_cnt == 1 && s_fixed_cnt == 0, "亮度 255 应恢复自动跟随（resume=%d fixed=%d）",
              s_resume_cnt, s_fixed_cnt);
}

/* ================================================================
 *  用例 4：显示并保存（SV）→ 图片槽
 * ================================================================ */

/**
 * @brief 显示并保存：按图片编号存槽并显示；编号越界 / 帧内位图不足 512B 都整帧丢弃
 *
 * "帧内位图够屏用"不等于"够填槽"：屏几何变小时前者可以更小，缺了后一条守卫，
 * `rls_pic_save` 的 memcpy 会越过帧缓冲。这里用小屏几何专门钉那条守卫。
 */
static void case_display_save(void)
{
    TEST_BEGIN("显示并保存：编号 0~2 存槽+显示，越界/位图不足丢弃");

    uint16_t n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 2, 2, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_save_cnt == 1 && s_pic_save_slot == 2 && s_pic_show_slot == 2,
              "编号 2 应存并显示槽 2（save=%d/%u show=%u）", s_pic_save_cnt,
              (unsigned)s_pic_save_slot, (unsigned)s_pic_show_slot);
    CHECK_MSG(s_pic_save_color == 2, "存进槽的颜色应是透传后的 2（绿），得到 %u",
              (unsigned)s_pic_save_color);

    /* 编号 3 越界：不存不显、也不渲染 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 1, 3, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_save_cnt == 0 && s_pic_show_cnt == 0 && s_draw_cnt == 0,
              "编号 3 必须整帧丢弃（save=%d show=%d）", s_pic_save_cnt, s_pic_show_cnt);

    /* 颜色越界：不存 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 9, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_save_cnt == 0, "颜色 9 越界时不得存槽（save=%d）", s_pic_save_cnt);

    /* 小屏几何（64×16 → 128B 就够屏）：帧内 128B 仍不够 512B 槽 → 拒存。
       注意 fixture_reset() 会把几何复位成 128×32，所以必须先 reset 再改几何。 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(0, 1, 0, 128));
    fixture_reset();
    s_rows = 64;
    s_cols = 16;
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_save_cnt == 0 && s_pic_show_cnt == 0,
              "帧内位图不足 512B 槽时必须拒存（save=%d show=%d）", s_pic_save_cnt, s_pic_show_cnt);

    /* SV 也要应用亮度（与 BM 同口径） */
    s_rows = 128;
    s_cols = 32;
    n      = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SAVE, s_disp, disp_payload(31, 1, 0, 512));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_fixed_cnt == 1 && s_fixed_last == 7, "SV 的亮度 31 也应落到 7 档，得到 %d 档",
              (unsigned)s_fixed_last);
}

/* ================================================================
 *  用例 5：显示已存储图片（DS）
 * ================================================================ */

/**
 * @brief 显示已存储图片：按编号显示槽；编号越界 / 短帧（<13）忽略；亮度照帧应用
 */
static void case_display_pic(void)
{
    TEST_BEGIN("显示已存储图片：按编号显示槽，越界与短帧忽略");

    uint16_t n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(0, 1, 1, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(n == 13 && s_pic_show_cnt == 1 && s_pic_show_slot == 1,
              "编号 1 应显示槽 1（帧长 %u，show=%d/%u）", (unsigned)n, s_pic_show_cnt,
              (unsigned)s_pic_show_slot);

    /* 编号 3 越界 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(0, 1, 3, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_pic_show_cnt == 0, "编号 3 越界必须忽略（show=%d）", s_pic_show_cnt);

    /* 短帧：整帧 12 字节（显示头都不全）→ 忽略。直接传长度，不经探针。 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(0, 1, 0, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, (uint16_t)(n - 1));
    CHECK_MSG(s_pic_show_cnt == 0, "12 字节短帧必须忽略（show=%d）", s_pic_show_cnt);

    /* 亮度应用 */
    n = frame_build(APP_RLS_CMD_TYPE_DISPLAY_PIC, s_disp, disp_payload(16, 1, 0, 0));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_fixed_cnt == 1 && s_fixed_last == 4, "DS 的亮度 16 应落到 4 档，得到 %d 档",
              (unsigned)s_fixed_last);
}

/* ================================================================
 *  用例 6：显示开关（55）
 * ================================================================ */

/**
 * @brief 显示开关：0x31 开、0x30 关，只切 OE 不动亮度；其它值/短帧忽略
 *
 * 拿"设亮度 0"实现关屏会把关屏前的档位丢掉，所以这里同时断言 `fixed/resume` 都是 0。
 */
static void case_display_switch(void)
{
    TEST_BEGIN("显示开关：0x31 开 / 0x30 关（只切 OE），其它值忽略");

    uint8_t  sw = 0x31;
    uint16_t n  = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(n == 10 && s_oe_on == 1 && s_oe_off == 0 && s_fixed_cnt == 0 && s_resume_cnt == 0,
              "0x31 应只开屏（on=%d off=%d fixed=%d）", s_oe_on, s_oe_off, s_fixed_cnt);

    sw = 0x30;
    n  = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_oe_on == 0 && s_oe_off == 1 && s_fixed_cnt == 0,
              "0x30 应只关屏（on=%d off=%d fixed=%d）", s_oe_on, s_oe_off, s_fixed_cnt);

    sw = 0x32; /* 协议外 */
    n  = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_oe_on == 0 && s_oe_off == 0, "0x32 应被忽略（on=%d off=%d）", s_oe_on, s_oe_off);

    /* 短帧 9 字节 → 忽略 */
    sw = 0x31;
    n  = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, (uint16_t)(n - 1));
    CHECK_MSG(s_oe_on == 0 && s_oe_off == 0, "9 字节短帧必须忽略（on=%d off=%d）", s_oe_on,
              s_oe_off);

    /* 开关命令回显：8 字节→10 字节帧，回原帧 */
    sw = 0x31;
    n  = frame_build(APP_RLS_CMD_TYPE_DISPLAY_SW, &sw, 1);
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == n, "开关命令应回显原帧（tx=%d len=%u）", s_tx_cnt,
              (unsigned)s_tx_len);
}

/* ================================================================
 *  用例 7：修改 IP（IP）
 * ================================================================ */

/**
 * @brief 修改 IP：12 字节地址落 IAP 记录；端口优先取记录、无记录回落 TCP 端口；短帧丢弃
 */
static void case_set_ip(void)
{
    TEST_BEGIN("修改 IP：写记录并保留端口，短帧丢弃");

    const uint8_t net[12] = {10, 1, 2, 3, 255, 255, 255, 0, 10, 1, 2, 1};
    uint16_t      n       = frame_build(APP_RLS_CMD_TYPE_SET_IP, net, sizeof(net));

    /* 记录有效：端口取记录里的 6001 */
    s_iap_rec_valid      = true;
    s_iap_rec.port       = 6001;
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_iap_upd_cnt == 1 && memcmp(s_iap_new_ip, net, 4) == 0 &&
                  memcmp(s_iap_new_mask, net + 4, 4) == 0 && memcmp(s_iap_new_gw, net + 8, 4) == 0,
              "应写入帧内 IP/掩码/网关（写 %d 次）", s_iap_upd_cnt);
    CHECK_MSG(s_iap_new_port == 6001, "端口应沿用记录里的 6001，得到 %u", (unsigned)s_iap_new_port);

    /* 无记录：回落 TCP 端口 */
    s_iap_rec_valid = false;
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);
    CHECK_MSG(s_iap_upd_cnt == 1 && s_iap_new_port == s_tcp_port,
              "无记录应回落 TCP 端口 %u，得到 %u", (unsigned)s_tcp_port,
              (unsigned)s_iap_new_port);

    /* 短帧 20 字节：整条丢弃 */
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, (uint16_t)(n - 1));
    CHECK_MSG(s_iap_upd_cnt == 0, "20 字节短帧必须丢弃（写 %d 次）", s_iap_upd_cnt);
}

/* ================================================================
 *  用例 8：搜索应答（ST/SC → RB）
 * ================================================================ */

/**
 * @brief 搜索应答逐字节：帧头 / 长度 / 命令码（线上 RB）/ 运行态地址 / BCC / 帧尾
 *
 * BCC 按文档正文"帧头到 BCC 之前所有字节异或"，即前 18 字节。
 */
static void case_report_ip(void)
{
    TEST_BEGIN("搜索应答：21 字节 RB，逐字节对拍（含 BCC）");

    const uint8_t net[12] = {0};
    uint16_t      n       = frame_build(APP_RLS_CMD_TYPE_AQUIRY_IP, net, sizeof(net));
    fixture_reset();
    app_rls_handle_frame(&s_ccb, s_frame, n);

    CHECK_MSG(s_tx_cnt == 1 && s_tx_len == RLS_REPORT_IP_LEN, "应回 1 帧 21 字节（%d/%u）",
              s_tx_cnt, (unsigned)s_tx_len);
    CHECK_MSG(s_tx_buf[0] == 0xFF && s_tx_buf[1] == 0xFE, "帧头应 FF FE（%02X %02X）",
              (unsigned)s_tx_buf[0], (unsigned)s_tx_buf[1]);
    CHECK_MSG(s_tx_buf[2] == 0x00 && s_tx_buf[3] == RLS_REPORT_IP_LEN,
              "长度域应大端 00 15（%02X %02X）", (unsigned)s_tx_buf[2], (unsigned)s_tx_buf[3]);
    CHECK_MSG(s_tx_buf[4] == 0x52 && s_tx_buf[5] == 0x42, "命令码应线上 RB（52 42），得到 %02X %02X",
              (unsigned)s_tx_buf[4], (unsigned)s_tx_buf[5]);
    CHECK_MSG(memcmp(&s_tx_buf[6], s_net_ip, 4) == 0 && memcmp(&s_tx_buf[10], s_net_mask, 4) == 0 &&
                  memcmp(&s_tx_buf[14], s_net_gw, 4) == 0,
              "地址域应是运行态 IP/掩码/网关");
    CHECK_MSG(s_tx_buf[18] == bcc_calc(s_tx_buf, 18), "BCC 应为前 18 字节异或（%02X，得到 %02X）",
              (unsigned)bcc_calc(s_tx_buf, 18), (unsigned)s_tx_buf[18]);
    CHECK_MSG(s_tx_buf[19] == 0x0D && s_tx_buf[20] == 0x0C, "帧尾应 0D 0C（%02X %02X）",
              (unsigned)s_tx_buf[19], (unsigned)s_tx_buf[20]);
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36mRLS 协议命令层（分派 / 回显 / 显示 / 图片槽 / 开关 / 网络）\033[0m\n");

    case_dispatch_matrix();
    case_echo_wire();
    case_display();
    case_display_save();
    case_display_pic();
    case_display_switch();
    case_set_ip();
    case_report_ip();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
