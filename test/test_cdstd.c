/**
 * @file    test_cdstd.c
 * @brief   CDSTD（折叠屏创迪标准协议）host 单测：探针四态 / 整帧等待 / CRC→FAKE、
 *          长度域与暂存区契约、跨协议互不毒化（IAP/CASC/RLS）、队列取帧与分派、
 *          命令层（A0 屏开关 / A1 文字显示 / A2 亮度 / A3 全屏填充 / A4 固化显示 /
 *          A7 图片显示 / A8 全屏固化显示 / A5 设置 IP / A6 上报 IP）与上电应用配置
 *
 * 命令层用例的写法契约见 docs/测试约定.md（桩、越界矩阵、断言预算、登记与验证清单）。
 *
 * **为什么需要这个测试**：探针是帧同步的唯一入口（同 test_casc_frame 的论证），
 * 判错一个分支的症状是"某类帧被吞掉"或"缓冲区只进不出"，都极难从现场反推。
 * 本协议另有三处**骨架落地时修复的控制流**，各由一条用例钉住：
 *   1. 队列取帧判断反了（真帧被丢、反拿未初始化的 msg 去查表）—— case_task_dispatch；
 *   2. 整帧到齐之前就比 CRC（分段到达的帧被当坏帧丢）—— case_wait_whole_frame；
 *   3. CRC 不过返回 SKIP（按伪长度把后面的真帧一起吞掉）—— case_crc_fail_is_fake。
 *
 * 另一重点是**互不毒化**：其它协议（IAP/CASC/RLS）的字节流不得被判成 CDSTD 帧；
 * （LDI 与 CDSTD 帧几何同构、帧层面不可区分，但两者不会同时启用，故不做该断言，见用例内注释。）
 *
 * 手法：用例 TU-include 生产 TU（探针、任务、模块资源与命令处理都是 static，外部够不着），
 * 命令层直接跑**真的** app_cdstd_cmd.c；应答用"发送记录桩"（app_ccb_send/_to）观察 ——
 * 命令表、拼帧、CRC 全是真的，测的就是它们本身。任务经真实 pl_task_new 跑在 pthread 上
 * （与 test_dispatch 同一手法）。
 *
 * 帧由用例独立构造（不复用探针的中间量），但按**帧结构体的字段布局**写入 ——
 * 结构体即线格式契约（与 test_probes 同口径）；CRC 尾按大端存放，与实现的读取一致。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_cdstd.h"
#include "app_cdstd_cfg.h"
#include "app_cdstd_cmd.h"
#include "app_dispatch.h"
#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "app_screen.h"
#include "app_udp.h"
#include "crc_utils.h"
#include "pl_net.h"
#include "ring_buffer.h"

/* ================================================================
 *  桩：模块 init 会碰到的通道访问器与绑定
 * ================================================================ */

static int s_bind_calls;

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
    s_bind_calls++;
}

app_ccb_t *app_udp_ccb(void) { return nullptr; }
app_ccb_t *app_rs485_ccb(void) { return nullptr; }

/* ---- 桩："上电应用"路径（假配置 + 记录网络调用） ----
 *  _cdstd_module_init 现在会读配置并应用 IP/端口（pl_net_set_ip 会触发 IAP 镜像）。
 *  默认无有效配置（不改网络）；用例把 s_fake_cfg_valid 置位即可观察应用结果。 */
static bool                s_fake_cfg_valid;
static app_cdstd_net_cfg_t s_fake_cfg;
static int                 s_net_set_calls;
static uint8_t             s_net_ip[4];
static uint8_t             s_net_mask[4];
static uint8_t             s_net_gw[4];
static int                 s_udp_port_calls;
static uint16_t            s_udp_port_set;

bool app_cdstd_load_config(app_cdstd_net_cfg_t *info)
{
    if (!s_fake_cfg_valid) return false;
    memcpy(info, &s_fake_cfg, sizeof(*info));
    return true;
}

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    s_net_set_calls++;
    memcpy(s_net_ip, ip, 4);
    memcpy(s_net_mask, mask, 4);
    memcpy(s_net_gw, gw, 4);
}

void app_udp_set_port(uint16_t port)
{
    s_udp_port_calls++;
    s_udp_port_set = port;
}

/** @brief 复位"上电应用"记录桩（docs/测试约定.md §4.4）：用例开头调用，避免用例间串味
 *
 *  包括把参数记录也清零：只清计数的话，断言失败时打印出来的"上一次残留值"
 *  会把现场指向错误的调用。 */
static void net_stub_reset(void)
{
    s_net_set_calls  = 0;
    s_udp_port_calls = 0;
    s_udp_port_set   = 0;
    memset(s_net_ip, 0, sizeof(s_net_ip));
    memset(s_net_mask, 0, sizeof(s_net_mask));
    memset(s_net_gw, 0, sizeof(s_net_gw));
}

/* ================================================================
 *  桩：命令层的保存 / IAP 镜像 / 运行态读取 / 应答发送（全部记录型）
 *
 *  命令层（app_cdstd_cmd.c）直接用真的 —— 它的命令表、拼帧、CRC 都是被测对象；
 *  这里替身的是它的外部依赖：
 *    · app_cdstd_save_config / app_flash_iap_update_net_cfg：记录入参，便于断言；
 *    · pl_net_get_ip / app_udp_get_port：提供"运行态"假值（0xA6 的回落分支用）；
 *    · app_ccb_send / _send_to：把发出去的应答录下来（状态应答走 send、B6 走 send_to）。
 * ================================================================ */

static int32_t s_save_ret; /**< 用例控制：0=成功 / 负=失败 */
static int     s_save_calls;
static uint8_t s_save_buf[sizeof(app_cdstd_net_cfg_t)];

int32_t app_cdstd_save_config(const app_cdstd_net_cfg_t *info)
{
    s_save_calls++;
    memcpy(s_save_buf, info, sizeof(*info));
    return s_save_ret;
}

static int      s_iap_calls;
static uint8_t  s_iap_ip[4];
static uint8_t  s_iap_mask[4];
static uint8_t  s_iap_gw[4];
static uint32_t s_iap_port;

void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    s_iap_calls++;
    memcpy(s_iap_ip, ip, 4);
    memcpy(s_iap_mask, mask, 4);
    memcpy(s_iap_gw, gw, 4);
    s_iap_port = port;
}

/** @brief 复位"保存 + IAP 镜像"记录桩（docs/测试约定.md §4.4）：用例开头调用
 *
 *  s_save_ret 一并归零：它带"失败注入"语义（-1 让 save 报错），
 *  残留到下一条用例会让下一条的"正常路径"变成失败路径。 */
static void cfg_stub_reset(void)
{
    s_save_ret   = 0;
    s_save_calls = 0;
    memset(s_save_buf, 0, sizeof(s_save_buf));
    s_iap_calls = 0;
    s_iap_port  = 0;
    memset(s_iap_ip, 0, sizeof(s_iap_ip));
    memset(s_iap_mask, 0, sizeof(s_iap_mask));
    memset(s_iap_gw, 0, sizeof(s_iap_gw));
}

static uint8_t  s_rt_ip[4]   = {192, 168, 1, 100}; /**< 运行态假值（0xA6 回落路径用） */
static uint8_t  s_rt_mask[4] = {255, 255, 255, 0};
static uint8_t  s_rt_gw[4]   = {192, 168, 1, 1};
static uint16_t s_rt_port    = 10011;

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_rt_ip, 4);
    memcpy(mask, s_rt_mask, 4);
    memcpy(gw, s_rt_gw, 4);
}

uint16_t app_udp_get_port(void) { return s_rt_port; }

static int      s_tx_calls;    /**< app_ccb_send（回源）次数 */
static int      s_tx_to_calls; /**< app_ccb_send_to（广播）次数 */
static uint16_t s_tx_len;
static uint8_t  s_tx_buf[64];

int32_t app_ccb_send(app_ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    s_tx_calls++;
    s_tx_len = len;
    if (len <= sizeof(s_tx_buf)) memcpy(s_tx_buf, data, len);
    return 0;
}

int32_t app_ccb_send_to(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    (void)dst;
    s_tx_to_calls++;
    s_tx_len = len;
    if (len <= sizeof(s_tx_buf)) memcpy(s_tx_buf, data, len);
    return 0;
}

/* ================================================================
 *  桩：渲染与整屏几何（命令层只"往逻辑屏画"，这里记录最后一次 cfg）
 *
 *  记录成散字段、不整结构拷贝 —— app_render_cfg_t 的成员带 const，C 里整结构赋值
 *  等于给只读成员赋值，编不过。几何桩给折叠屏部署的逻辑屏 224×100。
 * ================================================================ */

#define TEST_LOGICAL_W (224U) /**< 假逻辑屏宽（= app_screen_rows()） */
#define TEST_LOGICAL_H (100U) /**< 假逻辑屏高（= app_screen_cols()） */

uint16_t app_screen_rows(void) { return TEST_LOGICAL_W; }
uint16_t app_screen_cols(void) { return TEST_LOGICAL_H; }

/* ---- 帧单元颜色账的"新帧"声明 ----
 * 命令层在渲染前必须按协议矩形调一次（换色不残留旧帧色）。本套件不跑真实画布，
 * 只记录调用与矩形，验证"每条内容命令都声明、且矩形与渲染区域一致"。 */
static int      s_begin_calls;
static uint16_t s_begin_x, s_begin_y, s_begin_w, s_begin_h;

void app_screen_color_frame_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    s_begin_calls++;
    s_begin_x = x;
    s_begin_y = y;
    s_begin_w = w;
    s_begin_h = h;
}

static int                 s_render_calls;
static app_render_type_t   s_rc_type;
static uint16_t            s_rc_x;
static uint16_t            s_rc_y;
static uint16_t            s_rc_w;
static uint16_t            s_rc_h;
static dev_display_color_t s_rc_color;
static bool                s_rc_persist;

/* TEXT 分支专属（union 里其它分支的字节不当作文本字段读，见 docs/测试约定.md §4） */
static app_font_size_t  s_rc_font_size;
static app_font_type_t  s_rc_font_type;
static app_font_enc_t   s_rc_text_enc;
static const char      *s_rc_text;
static uint16_t         s_rc_len;
static app_render_style_t s_rc_style;

/* BITMAP 分支专属（A7 记录源指针，用来钉"位图数据从 DATA 第 3 字节开始"） */
static const uint8_t   *s_rc_bitmap;

void app_render(const app_render_cfg_t *cfg)
{
    s_render_calls++;
    s_rc_type    = cfg->type;
    s_rc_x       = cfg->x;
    s_rc_y       = cfg->y;
    s_rc_w       = cfg->w;
    s_rc_h       = cfg->h;
    s_rc_color   = cfg->color;
    s_rc_persist = cfg->persist;

    if (cfg->type == APP_RENDER_TYPE_TEXT) {
        s_rc_font_size = cfg->font_size;
        s_rc_font_type = cfg->font_type;
        s_rc_text_enc  = cfg->text_enc;
        s_rc_text      = cfg->text;
        s_rc_len       = cfg->len;
        s_rc_style     = cfg->style ? *cfg->style : (app_render_style_t){0};
    } else if (cfg->type == APP_RENDER_TYPE_BITMAP) {
        s_rc_bitmap = cfg->bitmap;
    }
}

static void render_reset(void)
{
    s_render_calls = 0;
    s_begin_calls  = 0;
    s_begin_x = s_begin_y = s_begin_w = s_begin_h = 0;
    s_rc_type      = (app_render_type_t)0;
    s_rc_x = s_rc_y = s_rc_w = s_rc_h = 0;
    s_rc_color   = DEV_DISPLAY_COLOR_BLACK;
    s_rc_persist = false;

    s_rc_font_size = APP_FONT_SIZE_SELF_ADAPT;
    s_rc_font_type = APP_FONT_TYPE_ST;
    s_rc_text_enc  = APP_FONT_ENC_ASCII;
    s_rc_text      = NULL;
    s_rc_len       = 0;
    s_rc_style     = (app_render_style_t){0};
    s_rc_bitmap    = NULL;
}

/* ================================================================
 *  桩：屏开关（OE）与光敏 —— A0/A2 命令层只"请求"，这里记录调用次数
 *
 *  A0 的口径是**不冻结**：关/开屏都不得调用光敏接口（一旦调用就说明回到了
 *  "冻结亮度"的旧口径，自动跟随状态下关屏会回不去），所以反例断言第一件事
 *  就是"光敏调用计数为 0"。
 * ================================================================ */

static bool    s_oe_state = true; /**< 门面输出使能（上电默认开） */
static uint8_t s_bright_hw = 7;   /**< app_screen_get_brightness() 的返回值（"当前有效亮度"） */
static int     s_fixed_calls;
static uint8_t s_fixed_level;
static int     s_resume_calls;
static int     s_oe_enable_calls;
static int     s_oe_disable_calls;

void app_screen_oe_enable(void)
{
    s_oe_enable_calls++;
    s_oe_state = true;
}

void app_screen_oe_disable(void)
{
    s_oe_disable_calls++;
    s_oe_state = false;
}

bool app_screen_is_oe_enabled(void) { return s_oe_state; }

uint8_t app_screen_get_brightness(void) { return s_bright_hw; }

void app_light_sensor_set_fixed(uint8_t level)
{
    s_fixed_calls++;
    s_fixed_level = level;
}

void app_light_sensor_resume(void)
{
    s_resume_calls++;
}

static void oe_stub_reset(bool on, uint8_t hw_level)
{
    s_oe_state         = on;
    s_bright_hw        = hw_level;
    s_fixed_calls      = 0;
    s_fixed_level      = 0;
    s_resume_calls     = 0;
    s_oe_enable_calls  = 0;
    s_oe_disable_calls = 0;
}

/* ================================================================ */
/*  被测：生产源码本体（与 test_casc_frame 同一手法）               */
/* ================================================================ */

#include "../Application/Src/CDSTD/app_cdstd.c"
#include "../Application/Src/CDSTD/app_cdstd_cmd.c" /* 命令层跑真的：表/拼帧/CRC 都是被测对象 */

/* ================================================================ */
/*  夹具                                                            */
/* ================================================================ */

/* 探针只用 self->rb；本套件自建大缓冲 —— 1400 字节杂物、回绕用例都要地方，
   生产的 1024 缓冲放不下这些场景。容量必须走 RB_DEFINE（裸 ring_buffer_t +
   rb_init 只绑锁、不设容量，size 为 0 会在取模时除零）。 */
RB_DEFINE(s_ut_rb, 4096);

static uint8_t s_frame[2048];
__attribute__((aligned(4))) static uint8_t s_scratch_buf[FRAME_DATA_MAX_LEN];

static void fixture_reset(void)
{
    s_ut_rb.read_index  = 0;
    s_ut_rb.write_index = 0;
    s_cdstd_pcb.rb      = &s_ut_rb;
}

static void cdstd_feed(const uint8_t *d, uint16_t n)
{
    rb_write(&s_ut_rb, d, n, nullptr);
}

static app_pcb_probe_state_t cdstd_probe(uint32_t *total_len, uint8_t *aux, uint16_t scratch_cap)
{
    return app_cdstd_probe_frame(&s_cdstd_pcb, nullptr, nullptr, s_scratch_buf, scratch_cap,
                                 total_len, aux);
}

/**
 * @brief 按帧结构体组一帧（独立构造，不复用探针中间量）
 *
 * LEN 字段 = plen + 1（含 CMD_TYPE，文档口径）；CRC16-XMODEM 覆盖 ver ~ DATA 尾
 * （长度 = plen + 7 = 6 + LEN），大端落在 DATA 之后 —— 与探针的读法一致。
 * 帧头/版本/序号对本套件无意义，填固定值。
 */
static uint16_t cdstd_build(uint8_t *out, uint8_t cmd, uint16_t plen, const void *payload)
{
    app_cdstd_frame_t *f = (app_cdstd_frame_t *)out;
    memset(out, 0, (size_t)sizeof(app_cdstd_frame_t) + plen + 2U);

    f->head[0] = 0xFF;
    f->head[1] = 0xFF;
    f->ver     = 0x01;
    f->seq     = 0x00;
    f->len[0]  = (uint8_t)((plen + 1U) >> 8);
    f->len[1]  = (uint8_t)(plen + 1U);
    f->cmd     = cmd;
    if (payload != nullptr && plen > 0) memcpy(&f->data_crc[0], payload, plen);

    uint16_t crc = crc16_xmodem(out + 2, (size_t)plen + sizeof(app_cdstd_frame_t) - 2U);
    f->data_crc[plen]     = (uint8_t)(crc >> 8);
    f->data_crc[plen + 1] = (uint8_t)(crc & 0xFF);

    return (uint16_t)(sizeof(app_cdstd_frame_t) + plen + 2U);
}

/* ================================================================
 *  断言与统计
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                 \
            printf("\n");                                                        \
        }                                                                        \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================ */

/** @brief 四态：不足 → WAIT、帧头错 → FAKE、完整 → READY、CRC 错 → FAKE */
static void case_probe_states(void)
{
    TEST_BEGIN("探针四态：WAIT / FAKE（帧头错）/ READY / FAKE（CRC 错）");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA3, 20, nullptr);
    CHECK_MSG(n == 31, "本用例帧长应为 31，得到 %u", (unsigned)n);

    /* 不足一个帧头 → WAIT */
    fixture_reset();
    cdstd_feed(s_frame, 5);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "只有 5 字节时应 WAIT");

    /* 帧头错 → FAKE */
    fixture_reset();
    uint8_t bad[64];
    memcpy(bad, s_frame, n);
    bad[0] = 0x00;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "帧头错应 FAKE");

    /* 完整 → READY：total_len / aux / 不消费都要对 */
    fixture_reset();
    cdstd_feed(s_frame, n);
    tl = 0;
    ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "完整帧应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(ax == (0xA3 & 0x0F), "aux 应为命令低 4 位（%02X），得到 %02X", 0xA3 & 0x0F, ax);
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == n, "探针只窥视，不得消费");

    /* 少最后一字节 → WAIT
       **反向验证**：去掉"整帧到齐"判断，这里会按半帧比 CRC、判成 FAKE */
    fixture_reset();
    cdstd_feed(s_frame, (uint16_t)(n - 1));
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "少最后一字节时应 WAIT（分段到达不能被当坏帧丢）");

    /* CRC 错 → FAKE */
    fixture_reset();
    memcpy(bad, s_frame, n);
    bad[n - 1] ^= 0x01;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "CRC 错应 FAKE");

    /* 载荷错一位（CRC 未跟着改）→ FAKE —— 这条才是 CRC 真正要抓的 */
    fixture_reset();
    memcpy(bad, s_frame, n);
    bad[10] ^= 0x01;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "载荷被改动应 FAKE");
}

/** @brief 分段到达：整帧到齐前必须 WAIT（不能按半帧比 CRC） */
static void case_wait_whole_frame(void)
{
    TEST_BEGIN("分段到达：整帧到齐前必须 WAIT，且不消费");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA1, 300, nullptr);
    CHECK_MSG(n == 311, "本用例帧长应为 311，得到 %u", (unsigned)n);

    fixture_reset();
    cdstd_feed(s_frame, 120); /* 帧头 + 长度域已到，整帧未到 */
    uint16_t before = rb_avail(&s_ut_rb, nullptr);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "整帧未到齐应 WAIT");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == before, "WAIT 不得消费");

    cdstd_feed(s_frame + 120, (uint16_t)(n - 120));
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "补齐后应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief 长度域：0 / 512 边界 READY，513 / 0xFFFF 越界 FAKE */
static void case_length_domain(void)
{
    TEST_BEGIN("长度域：0 / 1403 边界 READY，1404 / 0xFFFF 越界 FAKE");

    uint32_t tl = 0;
    uint8_t  ax = 0;

    /* 最短帧（DATA 域 0 字节）→ READY，total = 11 */
    fixture_reset();
    uint16_t n = cdstd_build(s_frame, 0xA0, 0, nullptr);
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "0 长度帧应 READY");
    CHECK_MSG(n == 11 && tl == 11, "最短帧应为 11 字节（构造 %u / 返回 %u）", (unsigned)n,
              (unsigned)tl);

    /* 最长帧（DATA 域 1403 = A8 的 3+1400）→ READY，total = 1414 */
    static uint8_t big[1403];
    memset(big, 0x5A, sizeof(big));
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 1403, big);
    CHECK_MSG(n == 1414, "最长帧应为 1414 字节，得到 %u", (unsigned)n);
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "1403 长度帧应 READY");
    CHECK_MSG(tl == 1414, "total_len 应为 1414，得到 %u", (unsigned)tl);

    /* 超上限：1404 → FAKE（帧头 + 长度域即可判定，不必等整帧） */
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 1404, nullptr);
    cdstd_feed(s_frame, 16);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "DATA 域 1404 超上限应 FAKE");

    /* 长度域 0xFFFF → FAKE（ASan 下不得越界读） */
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 0, nullptr);
    app_cdstd_frame_t *f = (app_cdstd_frame_t *)s_frame;
    f->len[0] = 0xFF;
    f->len[1] = 0xFF;
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "长度域 0xFFFF 应 FAKE");
}

/** @brief CRC 不过 → FAKE（不是 SKIP）；坏帧逐字节重跳后真帧仍能找回 */
static void case_crc_fail_is_fake(void)
{
    TEST_BEGIN("CRC 不过 → FAKE（不是 SKIP）：坏帧逐字节重跳，后面的真帧仍能找回");

    /* 一条"声明 512 字节、CRC 被破坏"的坏帧，内部 100 字节处嵌一条真帧。
       **反向验证**：把 CRC 失败改回 SKIP —— 框架会按伪长度（523）整段跳过，
       嵌入的真帧被连坐吞掉，下面的重跳循环找不到它。 */
    static uint8_t bad_payload[512];
    memset(bad_payload, 0x11, sizeof(bad_payload));

    uint16_t n_bad  = cdstd_build(s_frame, 0xA2, 512, bad_payload);
    uint8_t  good[64];
    uint16_t n_good = cdstd_build(good, 0xA5, 20, nullptr);
    CHECK_MSG(n_bad == 523 && n_good == 31, "构造帧长不对（%u / %u）", (unsigned)n_bad,
              (unsigned)n_good);

    memcpy(s_frame + 100, good, n_good);
    s_frame[n_bad - 1] ^= 0x01; /* 破坏 CRC 尾 */

    fixture_reset();
    cdstd_feed(s_frame, n_bad);

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "CRC 不过必须 FAKE —— 返回 SKIP 会按伪长度吞掉后面的字节");

    /* 模拟框架对 FAKE 的处置：跳 1 字节重探，直到认出嵌入的真帧 */
    bool found = false;
    for (int guard = 0; guard < 200; guard++) {
        app_pcb_probe_state_t st = cdstd_probe(&tl, &ax, sizeof(s_scratch_buf));
        if (st == APP_PCB_PROBE_STATE_READY) {
            found = true;
            break;
        }
        if (st != APP_PCB_PROBE_STATE_FAKE) break; /* WAIT / SKIP 都不该出现 */
        rb_skip(&s_ut_rb, 1, nullptr);
    }
    CHECK_MSG(found, "坏帧之后的重跳没能找回嵌入的真帧");
    CHECK_MSG(tl == n_good, "找回的应是嵌入真帧（%u），得到 %u", (unsigned)n_good, (unsigned)tl);
    CHECK_MSG(ax == (0xA5 & 0x0F), "aux 应为 %02X，得到 %02X", 0xA5 & 0x0F, ax);
}

/** @brief 暂存区契约：装不下 → SKIP 并报出整帧长度 */
static void case_scratch_contract(void)
{
    TEST_BEGIN("暂存区契约：装不下 → SKIP 且报出整帧长度");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA4, 512, nullptr);

    fixture_reset();
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, 16) == APP_PCB_PROBE_STATE_SKIP,
              "暂存区装不下应 SKIP（不是 READY，也不得越界写）");
    CHECK_MSG(tl == n, "SKIP 也要报出整帧长度（%u），得到 %u", (unsigned)n, (unsigned)tl);

    tl = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, n) == APP_PCB_PROBE_STATE_READY, "恰好装下应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief 定容契约：单次可写必须覆盖通道单段上限（RS485 一段可达 2048） */
static void case_rb_capacity_contract(void)
{
    TEST_BEGIN("定容契约：单次可写 ≥ 通道单段上限（RS485 一段可达 2048）");

    /* app_ccb_dispatch 把「传输层一整段」一次 rb_write 进来：装不下先 flush、再被
       rb_write 静默截短 —— 所以容量必须 ≥ 单次最大写入 + 1（环形缓冲留一个空槽
       区分满/空），否则 485 上多帧连发的一段会被从中间切掉。
       2048 = RS485_BUF_SIZE（app_rs485.c）：空闲交付一圈 DMA 的增量上限。
       **反向验证**：把 s_cdstd_rb 改小（如 1024），本用例两条断言立刻红。 */
    const uint32_t single_write_max = 2048U;
    const uint32_t frame_max = (uint32_t)sizeof(app_cdstd_frame_t) + CDSTD_PAYLOAD_MAX + 2U;

    CHECK_MSG((uint32_t)s_cdstd_rb.size >= single_write_max + 1U,
              "RB 容量 %u，装不下通道单段 %u 字节（需 ≥ %u）", (unsigned)s_cdstd_rb.size,
              (unsigned)single_write_max, (unsigned)(single_write_max + 1U));
    CHECK_MSG((uint32_t)s_cdstd_rb.size >= 2U * frame_max + 1U,
              "RB 容量 %u，不足 2 × 最长帧 %u + 1", (unsigned)s_cdstd_rb.size, (unsigned)frame_max);
}

/** @brief 伪帧不得把整帧拷进暂存区（逐字节重跳退化成 O(n²) 的守门） */
static void case_fake_must_not_copy_whole(void)
{
    TEST_BEGIN("伪帧不得拷整帧（框架逐字节重跳，否则退化成 O(n²) 的 memcpy）");

    static uint8_t junk[1400];
    memset(junk, 0x43, sizeof(junk)); /* 一个 0xFF 都不给 */

    fixture_reset();
    cdstd_feed(junk, sizeof(junk));
    memset(s_scratch_buf, 0xEE, sizeof(s_scratch_buf));

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "杂物应判 FAKE");

    /* 帧头/长度域最多占前 8 个字节，检查起点取 16 字节的宽松上界：
       起点以后必须原封不动（伪帧逐字节重跳，每次多拷一字节就是 O(n²)）。 */
    bool untouched = true;
    for (uint16_t i = 16; i < sizeof(s_scratch_buf); i++) {
        if (s_scratch_buf[i] != 0xEE) {
            untouched = false;
            break;
        }
    }
    CHECK_MSG(untouched, "FAKE 却把整帧拷进了暂存区 —— 逐字节重跳会退化成 O(n²)");
}

/** @brief 互不毒化：其它协议（IAP/CASC/RLS）的字节流不得被判成 CDSTD 帧 */
static void case_no_cross_poison(void)
{
    TEST_BEGIN("互不毒化：其它协议（IAP/CASC/RLS）的字节流不得判 CDSTD 帧");

    /* 注：LDI 与 CDSTD 的帧几何完全同构（STX、4 字节大端长度、CRC 覆盖 [2,6+L)、
       CRC 落位与字节序全一致），帧层面不可区分；两者不会同时启用（产品约束），
       故不做互不毒化断言。若将来需要共存，先在协议层引入区分手段（如命令域检查）。 */

    /* 其它协议的帧头都过不了 CDSTD 的帧头检查 → FAKE */
    static const struct {
        const char *name;
        uint8_t     head[4];
    } others[] = {
        {"IAP (5A5A5A5A)", {0x5A, 0x5A, 0x5A, 0x5A}},
        {"CASC (A55A)", {0xA5, 0x5A, 0x00, 0x00}},
        {"RLS (FFFE)", {0xFF, 0xFE, 0x00, 0x00}},
    };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        fixture_reset();
        uint8_t buf[64];
        memset(buf, 0x11, sizeof(buf));
        memcpy(buf, others[i].head, 4);
        cdstd_feed(buf, sizeof(buf));

        uint32_t tl2 = 0;
        uint8_t  ax2 = 0;
        CHECK_MSG(cdstd_probe(&tl2, &ax2, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
                  "%s 的字节流被判成 CDSTD 帧", others[i].name);
    }
}

/** @brief 一条帧分两段写入、且第二段让 RB 回绕 → 探针仍要能解析 */
static void case_split_across_rb_wrap(void)
{
    TEST_BEGIN("一条帧分两段写入、且第二段让 RB 回绕 → 仍要能解析");

    fixture_reset();
    uint16_t n = cdstd_build(s_frame, 0xA7, 300, nullptr);

    /* 先把读写指针推到"接近缓冲末尾"：这样第二段写必然让写指针绕回去。
       推进量按缓冲容量算，不写死 —— 换容量也不会越界。 */
    const uint16_t split   = (uint16_t)(n / 2);
    const uint16_t advance = (uint16_t)(s_ut_rb.size - 1U - split);

    static uint8_t junk[4096];
    static uint8_t sink[4096];
    memset(junk, 0xA5, sizeof(junk));
    cdstd_feed(junk, advance);
    CHECK_MSG(rb_read(&s_ut_rb, sink, advance, nullptr) == advance, "预置：把指针推到接近末尾");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == 0, "预置：缓冲应为空");

    const uint16_t w1 = rb_write(&s_ut_rb, s_frame, split, nullptr);
    const uint16_t w2 = rb_write(&s_ut_rb, s_frame + split, (uint16_t)(n - split), nullptr);
    CHECK_MSG(w1 == split && w2 == (uint16_t)(n - split), "两段都要完整写进去（%u + %u）",
              (unsigned)w1, (unsigned)w2);
    CHECK_MSG(s_ut_rb.write_index < s_ut_rb.read_index,
              "本用例要求第二段真的让写指针绕回（read=%u write=%u）—— 没绕回就测不到东西",
              (unsigned)s_ut_rb.read_index, (unsigned)s_ut_rb.write_index);

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "分两段写入 + RB 内部回绕之后探针认不出这条帧");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/* ================================================================
 *  命令层：A3 全屏填充 + 上电应用 + 0xA5 设置 IP / 0xA6 上报 IP
 * ================================================================ */

static void tx_reset(void)
{
    s_tx_calls    = 0;
    s_tx_to_calls = 0;
    s_tx_len      = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));
}

static bool wait_tx(int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (s_tx_calls + s_tx_to_calls >= want) return true;
        usleep(5000);
    }
    return s_tx_calls + s_tx_to_calls >= want;
}

/** @brief 造一条 A1 的 DATA（7 字节头 + text_len 字节文字），返回总字节数
 *
 *  头 7 字节的**偏移就是协议契约**（x 高/低、y 高/低、颜色、字体、字号），不随结构体漂移。 */
static uint16_t a1_payload(uint8_t *out, uint8_t color, uint8_t font_type, uint8_t font_size, uint16_t x,
                           uint16_t y, uint16_t text_len)
{
    const uint16_t hdr = (uint16_t)sizeof(app_cdstd_text_display_t);

    out[0] = (uint8_t)(x >> 8); /* 协议：先高八位、后低八位 */
    out[1] = (uint8_t)x;
    out[2] = (uint8_t)(y >> 8);
    out[3] = (uint8_t)y;
    out[4] = color;
    out[5] = font_type;
    out[6] = font_size;
    memset(out + hdr, 'A', text_len); /* 本层不做解码，填什么都可以 */
    return (uint16_t)(hdr + text_len);
}

/** @brief A1 文字显示：字号/字体映射表逐档 + 区域"左上角口径"
 *
 *  **为什么需要这个用例**：协议码序与工程枚举序**两处都不一致** —— 协议 0x30 是黑体，
 *  而本工程 `HT = 3`；协议字号是点阵数（16/24/32/48），也不是 0..3 的序号。
 *  所以两张映射表本身就是被测对象：把 `code - 0x30` 当档位用会画错字体（黑/宋互换），
 *  把 0x31 当"第二档"会画错字号。另一处是**区域口径**：x/y 是文字左上角，
 *  若沿用"水平居中 + 满屏宽"，x 会形同虚设（见 docs/测试约定.md §3）。 */
static void case_cmd_word_display(void)
{
    TEST_BEGIN("A1 文字显示：映射表逐档 / 区域左上角口径 / 文字指针与长度");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    uint8_t          payload[sizeof(app_cdstd_text_display_t) + 4];

    /* 正例：x=10 / y=20（大端）、红、黑体、48 点阵、"中"（GBK 双字节） */
    uint16_t n = a1_payload(payload, 0x31, 0x30, 0x33, 10, 20, 2);
    payload[sizeof(app_cdstd_text_display_t)]     = 0xD6;
    payload[sizeof(app_cdstd_text_display_t) + 1] = 0xD0;
    tx_reset();
    render_reset();
    _cdstd_word_display(&chan, payload, n);

    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB1 && s_tx_buf[9] == 0x31,
              "应回 0xB1/0x31（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_TEXT,
              "应恰好调一次 TEXT 渲染（calls=%d type=%d）", s_render_calls, (int)s_rc_type);
    /* 区域 = 剩余空间 [x,W)×[y,H)：w/h 是**尺寸**不是绝对右下边界 */
    CHECK_MSG(s_rc_x == 10 && s_rc_y == 20 && s_rc_w == TEST_LOGICAL_W - 10 &&
                  s_rc_h == TEST_LOGICAL_H - 20,
              "区域应为 (10,20,214,80)（得到 x=%u y=%u w=%u h=%u）", s_rc_x, s_rc_y, s_rc_w, s_rc_h);
    CHECK_MSG(s_begin_calls == 1 && s_begin_x == 10 && s_begin_y == 20 &&
                  s_begin_w == TEST_LOGICAL_W - 10 && s_begin_h == APP_FONT_SIZE_48,
              "渲染前应声明新帧且矩形=文字实际范围（calls=%d x=%u y=%u w=%u h=%u）", s_begin_calls,
              s_begin_x, s_begin_y, s_begin_w, s_begin_h);
    CHECK_MSG(s_rc_style.h_align == APP_RENDER_ALIGN_LEFT_UP &&
                  s_rc_style.v_align == APP_RENDER_ALIGN_LEFT_UP,
              "对齐应为左上（得到 h=%d v=%d）", (int)s_rc_style.h_align, (int)s_rc_style.v_align);
    /* 文字指针必须只指 DATA 尾部的文字（不含 7 字节头）；长度按字节给，不靠 \0 */
    CHECK_MSG(s_rc_text == (const char *)payload + sizeof(app_cdstd_text_display_t) && s_rc_len == 2 &&
                  (uint8_t)s_rc_text[0] == 0xD6 && (uint8_t)s_rc_text[1] == 0xD0,
              "文字应指向 DATA 尾部 2 字节（实际偏移=%ld len=%u）",
              (long)(s_rc_text - (const char *)payload), s_rc_len);
    CHECK_MSG(s_rc_color == DEV_DISPLAY_COLOR_RED && s_rc_font_size == APP_FONT_SIZE_48 &&
                  s_rc_font_type == APP_FONT_TYPE_HT && s_rc_text_enc == APP_FONT_ENC_GBK &&
                  s_rc_persist == false,
              "颜色/字号/字型/编码/persist 应为 RED/48/HT/GBK/0（得到 %u/%u/%u/%u/%d）",
              (unsigned)s_rc_color, (unsigned)s_rc_font_size, (unsigned)s_rc_font_type,
              (unsigned)s_rc_text_enc, (int)s_rc_persist);

    /* 映射表逐档：字号 0x30..0x33 = 16/24/32/48；字体 0x30..0x33 = 黑/仿宋/楷/宋 */
    static const struct {
        uint8_t         size_code;
        app_font_size_t size;
        uint8_t         type_code;
        app_font_type_t type;
    } map[] = {
        {0x30, APP_FONT_SIZE_16, 0x30, APP_FONT_TYPE_HT},
        {0x31, APP_FONT_SIZE_24, 0x31, APP_FONT_TYPE_FS},
        {0x32, APP_FONT_SIZE_32, 0x32, APP_FONT_TYPE_KT},
        {0x33, APP_FONT_SIZE_48, 0x33, APP_FONT_TYPE_ST},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        n = a1_payload(payload, 0x31, map[i].type_code, map[i].size_code, 0, 0, 1);
        tx_reset();
        render_reset();
        _cdstd_word_display(&chan, payload, n);
        CHECK_MSG(s_render_calls == 1 && s_rc_font_size == map[i].size && s_rc_font_type == map[i].type,
                  "字号码 %02X 应映射为 %u 点阵、字体码 %02X 应为 %u（得到 %u / %u）", map[i].size_code,
                  (unsigned)map[i].size, map[i].type_code, (unsigned)map[i].type,
                  (unsigned)s_rc_font_size, (unsigned)s_rc_font_type);
    }

    /* 全彩（2026-10-19 已定）：颜色 0x30..0x37 逐档 —— 黑/红/绿/黄/蓝/紫/青/白 */
    static const struct {
        uint8_t             code;
        dev_display_color_t want;
    } colors[] = {
        {0x30, DEV_DISPLAY_COLOR_BLACK}, {0x31, DEV_DISPLAY_COLOR_RED},
        {0x32, DEV_DISPLAY_COLOR_GREEN}, {0x33, DEV_DISPLAY_COLOR_YELLOW},
        {0x34, DEV_DISPLAY_COLOR_BLUE},  {0x35, DEV_DISPLAY_COLOR_PURPLE},
        {0x36, DEV_DISPLAY_COLOR_CYAN},  {0x37, DEV_DISPLAY_COLOR_WHITE},
    };
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        n = a1_payload(payload, colors[i].code, 0x30, 0x30, 0, 0, 1);
        tx_reset();
        render_reset();
        _cdstd_word_display(&chan, payload, n);
        CHECK_MSG(s_render_calls == 1 && s_rc_color == colors[i].want,
                  "颜色码 %02X 应映射为 %u（全彩；得到 %u）", colors[i].code,
                  (unsigned)colors[i].want, (unsigned)s_rc_color);
    }
}

/** @brief A1 越界矩阵：字段低位/高位/边界一律 0xB1/0x30，且**零副作用**
 *
 *  **为什么需要**：低位（< 0x30）这个文件的历次缺陷都出在这里 —— 原版
 *  `_table[code - 0x30]` 直接算负下标（数组越界读，ASan 可抓），颜色判据又写在"未转换的
 *  表达式"上（负 int 不满足 `> WHITE` → 漏放行）。前两行留着当回归哨兵。 */
static void case_cmd_word_display_bounds(void)
{
    TEST_BEGIN("A1 越界矩阵：映射越界 / 长度边界 / 坐标越界 → 0x30 且不渲染");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    uint8_t          payload[sizeof(app_cdstd_text_display_t) + 4];

    /* 长度维度：比 7 字节头还少 1 字节（头部截断）→ 0xB1/0x30 且不调 render。
       表里第一行钉的是"恰好头部"（只差文字）；少一字节时连尾部字段都不在
       data_len 之内 —— 少了这条，`data_len - sizeof()` 类下溢会带着越界读通过。 */
    tx_reset();
    render_reset();
    _cdstd_word_display(&chan, payload, (uint16_t)(sizeof(app_cdstd_text_display_t) - 1U));
    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB1 && s_tx_buf[9] == 0x30,
              "头部少 1 字节应回 0xB1/0x30（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 0, "头部截断不得调 render（calls=%d）", s_render_calls);

    static const struct {
        uint8_t     color, type, size;
        uint16_t    x, y, text_len;
        const char *why;
    } bad[] = {
        {0x31, 0x30, 0x33, 10, 20, 0, "只有 7 字节头、没带文字"},
        {0x29, 0x30, 0x33, 10, 20, 1, "颜色码低于 0x30（曾漏放行）"},
        {0x38, 0x30, 0x33, 10, 20, 1, "颜色码高于 0x37"},
        {0x31, 0x29, 0x33, 10, 20, 1, "字体码低于 0x30"},
        {0x31, 0x38, 0x33, 10, 20, 1, "字体码高于 0x33"},
        {0x31, 0x30, 0x29, 10, 20, 1, "字号码低于 0x30（曾是负下标越界读）"},
        {0x31, 0x30, 0x38, 10, 20, 1, "字号码高于 0x33"},
        {0x31, 0x30, 0x33, TEST_LOGICAL_W, 20, 1, "x 等于屏宽（区域宽为 0）"},
        {0x31, 0x30, 0x33, 10, TEST_LOGICAL_H, 1, "y 等于屏高"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint16_t n = a1_payload(payload, bad[i].color, bad[i].type, bad[i].size, bad[i].x, bad[i].y,
                                bad[i].text_len);
        tx_reset();
        render_reset();
        _cdstd_word_display(&chan, payload, n);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB1 && s_tx_buf[9] == 0x30,
                  "越界（%s）应回 0xB1/0x30（得到 cmd=%02X status=%02X）", bad[i].why, s_tx_buf[8],
                  s_tx_buf[9]);
        CHECK_MSG(s_render_calls == 0, "越界（%s）不得调 render（calls=%d）", bad[i].why, s_render_calls);
    }
}

/** @brief A1 大端边界：y 取"界内最后一位"（99 = 0x0063），验证高低字节没接反
 *
 *  **为什么需要**：x/y 按"先高八位、后低八位"合成为 `(hi << 8) | lo`。y 的高字节
 *  0x00 / 低字节 0x63 若接反，y 会变成 0x6300 = 25344，被"y >= 屏高"立刻拦成 0x30；
 *  取 y=20 这种小值时两种合成都在界内，接反测不出来。99 是 `y < 100` 的最后一位，
 *  同时把"区域高 = 屏高 - y"逼到最薄的一格（h=1），区域口径错一位就红。 */
static void case_cmd_word_display_endian(void)
{
    TEST_BEGIN("A1 大端边界：y=99（字节 0x00/0x63）→ y=99、区域高=1");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    uint8_t          payload[sizeof(app_cdstd_text_display_t) + 1];

    uint16_t n = a1_payload(payload, 0x31, 0x30, 0x33, 10, TEST_LOGICAL_H - 1U, 1);
    CHECK_MSG(payload[2] == 0x00 && payload[3] == (TEST_LOGICAL_H - 1U),
              "构造前提：y 的大端字节应为 0x00/%02X（得到 %02X/%02X）", TEST_LOGICAL_H - 1U,
              payload[2], payload[3]);

    tx_reset();
    render_reset();
    _cdstd_word_display(&chan, payload, n);

    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB1 && s_tx_buf[9] == 0x31,
              "界内最后一位应成功（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_y == TEST_LOGICAL_H - 1U,
              "y 应为 99（字节序接反会得到 25344 并被拦下），得到 calls=%d y=%u", s_render_calls,
              (unsigned)s_rc_y);
    CHECK_MSG(s_rc_h == 1, "区域高应为 屏高-99 = 1（w/h 是尺寸不是右下边界），得到 %u",
              (unsigned)s_rc_h);
}

/** @brief A0 屏开关：**只管输出**（亮度期望值与光敏跟随都不动），且幂等
 *
 *  **为什么需要**：早先设计要求"关屏时冻结亮度"（先停光敏），但那样"自动跟随状态下
 *  关屏、再开屏"就**再也回不到自动跟随**了。现行口径是**不冻结**：开关只切 OE 输出，
 *  期望亮度继续由既有来源维护（自动跟随照常每秒更新），于是开屏恢复的是"当时的亮度
 *  与当时的模式"。断言因此钉两件事：① 关/开屏都**不得调用光敏接口**（调了就回到冻结
 *  口径）；② 重复关/开屏**幂等**（不产生多余的级联广播）。 */
static void case_cmd_screen_switch(void)
{
    TEST_BEGIN("A0 屏开关：只切输出(不冻结亮度/不动光敏) / 幂等");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    uint8_t          v    = 0x30;

    /* 关屏：撤输出，且不碰光敏（亮度期望值原样留着，自动跟随照常跑） */
    tx_reset();
    oe_stub_reset(true, 5);
    _cdstd_screen_switch(&chan, &v, 1);
    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB0 && s_tx_buf[9] == 0x31,
              "应回 0xB0/0x31（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_oe_state == false && s_oe_disable_calls == 1, "应撤掉输出（oe_disable 一次）");
    CHECK_MSG(s_fixed_calls == 0 && s_resume_calls == 0,
              "关屏**不得动光敏**（否则自动跟随下关屏就回不去了）：fixed=%d resume=%d",
              s_fixed_calls, s_resume_calls);

    /* 重复关屏：幂等 —— 不得再调 oe_disable（每次都置"待下发"，重复就多一次广播） */
    tx_reset();
    _cdstd_screen_switch(&chan, &v, 1);
    CHECK_MSG(s_tx_buf[8] == 0xB0 && s_tx_buf[9] == 0x31, "重复关屏应回 0xB0/0x31");
    CHECK_MSG(s_oe_disable_calls == 1, "重复关屏不应再调 oe_disable（得到 %d 次）",
              s_oe_disable_calls);

    /* 开屏：使能输出，同样不碰光敏（模式由 A2 决定，开关不改它） */
    tx_reset();
    v = 0x31;
    _cdstd_screen_switch(&chan, &v, 1);
    CHECK_MSG(s_tx_buf[8] == 0xB0 && s_tx_buf[9] == 0x31, "开屏应回 0xB0/0x31");
    CHECK_MSG(s_oe_state == true && s_oe_enable_calls == 1, "开屏应使能输出（oe_enable 一次）");
    CHECK_MSG(s_fixed_calls == 0 && s_resume_calls == 0, "开屏也不得动光敏（模式由 A2 决定）");
}

/** @brief A0 越界矩阵：长度≠1 与非法取值一律 0xB0/0x30，且**零副作用**
 *
 *  **为什么需要**：这类"只回状态码"的路径最容易写成先动状态再校验；顺序一反，
 *  一条坏帧也会把关屏冻结值搅坏。所以反例统一断言"三件事都没发生"。 */
static void case_cmd_screen_switch_bounds(void)
{
    TEST_BEGIN("A0 越界矩阵：长度≠1 / 取值非法 → 0xB0/0x30 且零副作用");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static const struct {
        uint16_t    len;
        uint8_t     code;
        const char *why;
    } bad[] = {
        {0, 0x30, "没有 DATA"},
        {2, 0x30, "多一个字节"},
        {1, 0x2F, "低于 0x30"},
        {1, 0x32, "高于 0x31"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t buf[2] = {bad[i].code, 0x00};
        tx_reset();
        oe_stub_reset(true, 5);
        _cdstd_screen_switch(&chan, buf, bad[i].len);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB0 && s_tx_buf[9] == 0x30,
                  "越界（%s）应回 0xB0/0x30（得到 cmd=%02X status=%02X）", bad[i].why, s_tx_buf[8],
                  s_tx_buf[9]);
        CHECK_MSG(s_fixed_calls == 0 && s_oe_enable_calls == 0 && s_oe_disable_calls == 0 &&
                      s_oe_state == true,
                  "越界（%s）不得产生任何副作用（fixed=%d oe_en=%d oe_off=%d state=%d）",
                  bad[i].why, s_fixed_calls, s_oe_enable_calls, s_oe_disable_calls,
                  (int)s_oe_state);
    }
}

/** @brief A2 亮度：0x30 走自动跟随、0x31..0x38 落固定档（第 8 档由门面钳到 7）
 *
 *  **为什么需要**：两条分支只差一个边界（0x30 是"自动"、不是"0 档"），写错就会
 *  **"发自动调光 → 屏黑掉"**。`0x38` 是协议第 8 档，命令层**原样**传 `code - 0x30`；
 *  钳位归门面（`app_screen_set_brightness()` 的 `>7 → 7`），其行为由
 *  `test_screen_brightness` 的钳位用例覆盖，本层不重复钳。 */
static void case_cmd_light_adjust(void)
{
    TEST_BEGIN("A2 亮度：0x30=自动跟随 / 0x31..0x38 固定档（8 由门面钳到 7）");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static const struct {
        uint8_t     code;
        uint8_t     want;
        const char *why;
    } fixed[] = {
        {0x31, 1, "最暗档"},
        {0x34, 4, "中间档"},
        {0x37, 7, "最亮档"},
        {0x38, 8, "第 8 档（原样传 8，门面钳到 7）"},
    };

    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        uint8_t v = fixed[i].code;
        tx_reset();
        oe_stub_reset(true, 7);
        _cdstd_light_adjust(&chan, &v, 1);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB2 && s_tx_buf[9] == 0x31,
                  "%s（%02X）应回 0xB2/0x31（得到 %02X/%02X）", fixed[i].why, fixed[i].code,
                  s_tx_buf[8], s_tx_buf[9]);
        CHECK_MSG(s_fixed_calls == 1 && s_fixed_level == fixed[i].want && s_resume_calls == 0,
                  "%s（%02X）应请求固定档 %u（得到 fixed=%d level=%u resume=%d）", fixed[i].why,
                  fixed[i].code, (unsigned)fixed[i].want, s_fixed_calls,
                  (unsigned)s_fixed_level, s_resume_calls);
    }

    /* 0x30 = 自动调光：必须走 resume()，**绝不能** set_fixed(0)（那等于关屏） */
    uint8_t v = 0x30;
    tx_reset();
    oe_stub_reset(true, 7);
    _cdstd_light_adjust(&chan, &v, 1);
    CHECK_MSG(s_tx_buf[8] == 0xB2 && s_tx_buf[9] == 0x31, "0x30 应回 0xB2/0x31");
    CHECK_MSG(s_resume_calls == 1 && s_fixed_calls == 0,
              "0x30 必须恢复自动跟随（resume=1、set_fixed=0；set_fixed(0) 会把屏关掉）");
}

/** @brief A2 越界矩阵：长度≠1 与取值出界一律 0xB2/0x30 且零副作用
 *
 *  **为什么需要**：出界值若被放过，会经 `code - 0x30` 回绕成大数再被钳到最亮 ——
 *  表现是"发乱码反而跳到最亮"，而且回的是成功。 */
static void case_cmd_light_adjust_bounds(void)
{
    TEST_BEGIN("A2 越界矩阵：长度≠1 / 取值出界 → 0xB2/0x30 且零副作用");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static const struct {
        uint16_t    len;
        uint8_t     code;
        const char *why;
    } bad[] = {
        {0, 0x31, "没有 DATA"},
        {2, 0x31, "多一个字节"},
        {1, 0x00, "取值 0"},
        {1, 0x2F, "低于 0x30"},
        {1, 0x39, "高于 0x38"},
        {1, 0xFF, "0xFF"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t buf[2] = {bad[i].code, 0x00};
        tx_reset();
        oe_stub_reset(true, 7);
        _cdstd_light_adjust(&chan, buf, bad[i].len);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB2 && s_tx_buf[9] == 0x30,
                  "越界（%s）应回 0xB2/0x30（得到 cmd=%02X status=%02X）", bad[i].why, s_tx_buf[8],
                  s_tx_buf[9]);
        CHECK_MSG(s_fixed_calls == 0 && s_resume_calls == 0,
                  "越界（%s）不得碰亮度（fixed=%d resume=%d）", bad[i].why, s_fixed_calls,
                  s_resume_calls);
    }
}

/** @brief A4 上下屏固化显示：与 A1 同构，但**必须带 persist**（固化 = 落盘）
 *
 *  **为什么需要**：A4 的 DATA 与 A1 逐字节同构（协议 4.5 节：7+N），唯一区别是语义
 *  "固化"——若实现里漏了 `persist = true`，命令会"成功"但重启即丢，现场只会觉得
 *  "偶尔不保存"，极难反查。 */
static void case_cmd_persistent_display(void)
{
    TEST_BEGIN("A4 固化显示：与 A1 同构 + persist=true");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    uint8_t          payload[sizeof(app_cdstd_text_display_t) + 2];

    uint16_t n = a1_payload(payload, 0x31, 0x30, 0x33, 10, 20, 2);
    tx_reset();
    render_reset();
    _cdstd_persistent_display(&chan, payload, n);

    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB4 && s_tx_buf[9] == 0x31,
              "应回 0xB4/0x31（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_TEXT && s_rc_persist == true,
              "必须是 TEXT 且 **persist=true**（calls=%d persist=%d）", s_render_calls,
              (int)s_rc_persist);
    CHECK_MSG(s_rc_x == 10 && s_rc_y == 20 && s_rc_color == DEV_DISPLAY_COLOR_RED &&
                  s_rc_font_size == APP_FONT_SIZE_48 && s_rc_font_type == APP_FONT_TYPE_HT,
              "区域/颜色/字号/字型应与 A1 同口径（得到 x=%u y=%u color=%u size=%u type=%u）",
              s_rc_x, s_rc_y, (unsigned)s_rc_color, (unsigned)s_rc_font_size,
              (unsigned)s_rc_font_type);
    CHECK_MSG(s_begin_calls == 1 && s_begin_x == 10 && s_begin_y == 20 &&
                  s_begin_w == TEST_LOGICAL_W - 10 && s_begin_h == APP_FONT_SIZE_48,
              "A4 渲染前也应声明新帧（calls=%d x=%u y=%u w=%u h=%u）", s_begin_calls, s_begin_x,
              s_begin_y, s_begin_w, s_begin_h);

    /* 反例：只有 7 字节头（无文字）→ 0xB4/0x30 且不渲染（A1 的口径，A4 必须一致） */
    tx_reset();
    render_reset();
    _cdstd_persistent_display(&chan, payload, sizeof(app_cdstd_text_display_t));
    CHECK_MSG(s_tx_buf[8] == 0xB4 && s_tx_buf[9] == 0x30, "无文字应回 0xB4/0x30");
    CHECK_MSG(s_render_calls == 0, "无文字不得调 render");
}

/** @brief A7 图片显示：只接受 pos=1/2（全屏单帧装不下）、落半边、位图指针从 DATA 第 3 字节起
 *
 *  **为什么需要**：A7 的位图按 1bpp 逐行排（行字节 = ceil(w/8)），**半屏正好 1400 字节**；
 *  pos=0（全屏）需要 2800 字节，超出本协议单帧上限（1403），必须明确拒绝而不是画半张；
 *  pos≥3 非法。`persist=true` 是刻意行为（A7 与 A8 是两种来源的固化显示）。 */
static void case_cmd_picture_display(void)
{
    TEST_BEGIN("A7 图片显示：上屏/下屏落位 + persist + 位图指针");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    /* 头部 2 字节 + 1401 字节位图：正常帧用 1402（半屏正好），最后一组用 1403 测"多带" */
    static uint8_t payload[sizeof(app_cdstd_picture_display_t) + 1401];
    const uint16_t exact = (uint16_t)(sizeof(app_cdstd_picture_display_t) + 1400);

    memset(payload + sizeof(app_cdstd_picture_display_t), 0xA5, 1401);
    payload[0] = 1;    /* pos = 上屏 */
    payload[1] = 0x31; /* 红 */

    tx_reset();
    render_reset();
    _cdstd_picture_display(&chan, payload, exact);
    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB7 && s_tx_buf[9] == 0x31,
              "上屏应回 0xB7/0x31（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_BITMAP && s_rc_x == 0 &&
                  s_rc_y == 0 && s_rc_w == TEST_LOGICAL_W && s_rc_h == TEST_LOGICAL_H / 2 &&
                  s_rc_color == DEV_DISPLAY_COLOR_RED && s_rc_persist == true,
              "上屏应为 BITMAP(0,0,224,50) 红且 persist（type=%d y=%u h=%u color=%u persist=%d）",
              (int)s_rc_type, s_rc_y, s_rc_h, (unsigned)s_rc_color, (int)s_rc_persist);
    CHECK_MSG(s_begin_calls == 1 && s_begin_x == 0 && s_begin_y == 0 &&
                  s_begin_w == TEST_LOGICAL_W && s_begin_h == TEST_LOGICAL_H / 2,
              "位图渲染前应声明新帧 (0,0,224,50)（calls=%d x=%u y=%u w=%u h=%u）", s_begin_calls,
              s_begin_x, s_begin_y, s_begin_w, s_begin_h);
    CHECK_MSG(s_rc_bitmap == payload + sizeof(app_cdstd_picture_display_t),
              "位图指针必须指向 DATA 第 3 字节（偏移 %ld）", (long)(s_rc_bitmap - payload));

    /* 下屏：pos=2 → 起点下移半屏，高度仍是半屏 */
    payload[0] = 2;
    tx_reset();
    render_reset();
    _cdstd_picture_display(&chan, payload, exact);
    CHECK_MSG(s_tx_buf[8] == 0xB7 && s_tx_buf[9] == 0x31, "下屏应回 0xB7/0x31");
    CHECK_MSG(s_render_calls == 1 && s_rc_y == TEST_LOGICAL_H / 2 && s_rc_h == TEST_LOGICAL_H / 2,
              "下屏起点应为 y=%u、高 %u（得到 y=%u h=%u）", TEST_LOGICAL_H / 2, TEST_LOGICAL_H / 2,
              s_rc_y, s_rc_h);

    /* 全彩：0x35 紫 → 同样落半屏（颜色不限于红/绿/黄） */
    payload[0] = 1;
    payload[1] = 0x35;
    tx_reset();
    render_reset();
    _cdstd_picture_display(&chan, payload, exact);
    CHECK_MSG(s_tx_buf[9] == 0x31 && s_render_calls == 1 &&
                  s_rc_color == DEV_DISPLAY_COLOR_PURPLE,
              "0x35 应映射为紫（全彩，得到 %u）", (unsigned)s_rc_color);

    /* 多带 1 字节：口径取 ≥，多带不拒（多余字节不画） */
    tx_reset();
    render_reset();
    _cdstd_picture_display(&chan, payload, (uint16_t)sizeof(payload));
    CHECK_MSG(s_tx_buf[9] == 0x31 && s_render_calls == 1, "多带 1 字节不应拒（口径 ≥）");
}

/** @brief A7 越界矩阵：短帧 / pos=0 或 ≥3 / 颜色非法 → 0xB7/0x30 且不渲染
 *
 *  **为什么需要**：短帧最危险 —— 位图按几何读，缺字节就是**越界读帧缓冲**（与 RLS
 *  那条守卫同一类缺陷）。pos=0 是**能力边界**而非非法值：全屏 2800 字节单帧装不下，
 *  必须拒绝并让上位机改发两包。 */
static void case_cmd_picture_display_bounds(void)
{
    TEST_BEGIN("A7 越界矩阵：短帧 / pos=0 / pos≥3 / 颜色非法 → 0xB7/0x30 且不渲染");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static uint8_t   payload[sizeof(app_cdstd_picture_display_t) + 1400];

    memset(payload + sizeof(app_cdstd_picture_display_t), 0xA5, 1400);
    payload[1] = 0x31; /* 红 */

    static const struct {
        uint8_t     pos;
        uint16_t    len;
        const char *why;
    } bad[] = {
        {1, (uint16_t)(sizeof(app_cdstd_picture_display_t) + 1399), "半屏少 1 字节"},
        {1, (uint16_t)sizeof(app_cdstd_picture_display_t), "只有头、没有位图"},
        {0, (uint16_t)sizeof(payload), "pos=0 全屏（单帧装不下）"},
        {3, (uint16_t)sizeof(payload), "pos=3 非法"},
        {2, (uint16_t)(sizeof(app_cdstd_picture_display_t) + 700), "下半屏只有一半位图"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        payload[0] = bad[i].pos;
        tx_reset();
        render_reset();
        _cdstd_picture_display(&chan, payload, bad[i].len);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB7 && s_tx_buf[9] == 0x30,
                  "越界（%s）应回 0xB7/0x30（得到 cmd=%02X status=%02X）", bad[i].why, s_tx_buf[8],
                  s_tx_buf[9]);
        CHECK_MSG(s_render_calls == 0, "越界（%s）不得调 render", bad[i].why);
    }

    /* 颜色非法：0x38（映射后 > WHITE）。0x30..0x37 是全彩口径（已定），0x38 起才越界 */
    payload[0] = 1;
    payload[1] = 0x38;
    tx_reset();
    render_reset();
    _cdstd_picture_display(&chan, payload, (uint16_t)sizeof(payload));
    CHECK_MSG(s_tx_buf[9] == 0x30 && s_render_calls == 0, "颜色 0x38 应拒绝（与 A1/A3 同口径）");
}

/** @brief A8 全屏固化显示：两包各落半屏、**无反馈**、定长 DATA
 *
 *  **为什么需要**：A8 与 A7 都是图片固化，但来源不同（A8 给发布盒），且协议 4.9 明确
 *  **无反馈** —— 非法帧只能静默丢弃。所以断言必须同时钉"画了没有"和"发没发帧"。
 *  seq 按协议是 0x01/0x02（1=上半屏、2=下半屏），DATA 定长 1403（3 + 1400）。 */
static void case_cmd_release_display(void)
{
    TEST_BEGIN("A8 固化显示：0x01 上屏 / 0x02 下屏、persist、无反馈");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static uint8_t   payload[sizeof(app_cdstd_release_box_t)];

    memset(payload, 0xA5, sizeof(payload));
    payload[0] = 0x01; /* 包序号：上半屏 */
    payload[1] = 0x01; /* 固化编号：保留不用（任意值都行） */
    payload[2] = 0x31; /* 红 */

    tx_reset();
    render_reset();
    _cdstd_release_display(&chan, payload, sizeof(payload));
    CHECK_MSG(s_tx_calls == 0, "协议 4.9 无反馈：本命令不得发任何帧（实际 %d 次）", s_tx_calls);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_BITMAP && s_rc_x == 0 &&
                  s_rc_y == 0 && s_rc_w == TEST_LOGICAL_W && s_rc_h == TEST_LOGICAL_H / 2 &&
                  s_rc_color == DEV_DISPLAY_COLOR_RED && s_rc_persist == true,
              "上包应为 BITMAP(0,0,224,50) 红且 persist（type=%d y=%u h=%u persist=%d）",
              (int)s_rc_type, s_rc_y, s_rc_h, (int)s_rc_persist);
    CHECK_MSG(s_rc_bitmap == payload + 3, "位图必须从 DATA 第 4 字节起（偏移 %ld）",
              (long)(s_rc_bitmap - payload));

    /* 下包：seq=0x02 → 起点下移半屏 */
    payload[0] = 0x02;
    tx_reset();
    render_reset();
    _cdstd_release_display(&chan, payload, sizeof(payload));
    CHECK_MSG(s_tx_calls == 0 && s_render_calls == 1 && s_rc_y == TEST_LOGICAL_H / 2,
              "下包应为 y=%u（得到 y=%u，tx=%d）", TEST_LOGICAL_H / 2, s_rc_y, s_tx_calls);

    /* 全彩：0x34 蓝 */
    payload[0] = 0x01;
    payload[2] = 0x34;
    tx_reset();
    render_reset();
    _cdstd_release_display(&chan, payload, sizeof(payload));
    CHECK_MSG(s_tx_calls == 0 && s_render_calls == 1 && s_rc_color == DEV_DISPLAY_COLOR_BLUE,
              "0x34 应映射为蓝（全彩，得到 %u）", (unsigned)s_rc_color);
}

/** @brief A8 越界矩阵：长度、seq 非法、颜色非法 → **静默丢弃**（不画、不发帧）
 *
 *  **为什么需要**：协议无反馈，错误路径唯一的可观测量就是"什么都没发生"；长度尤其关键
 *  —— 定长结构少一字节就说明帧不对，绝不能拿去画（会越界读）。 */
static void case_cmd_release_display_bounds(void)
{
    TEST_BEGIN("A8 越界矩阵：长度/seq/颜色非法 → 静默丢弃（不画、不发帧）");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};
    static uint8_t   payload[sizeof(app_cdstd_release_box_t) + 1];

    memset(payload, 0xA5, sizeof(payload));
    payload[2] = 0x31;

    static const struct {
        uint8_t     seq;
        uint16_t    len;
        uint8_t     color;
        const char *why;
    } bad[] = {
        {0x01, (uint16_t)(sizeof(app_cdstd_release_box_t) - 1), 0x31, "少 1 字节"},
        {0x01, (uint16_t)(sizeof(app_cdstd_release_box_t) + 1), 0x31, "多 1 字节"},
        {0x00, (uint16_t)sizeof(app_cdstd_release_box_t), 0x31, "seq=0"},
        {0x03, (uint16_t)sizeof(app_cdstd_release_box_t), 0x31, "seq=3"},
        {0x31, (uint16_t)sizeof(app_cdstd_release_box_t), 0x31, "seq=0x31（旧错值）"},
        {0x01, (uint16_t)sizeof(app_cdstd_release_box_t), 0x38, "颜色 0x38"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        payload[0] = bad[i].seq;
        payload[2] = bad[i].color;
        tx_reset();
        render_reset();
        _cdstd_release_display(&chan, payload, bad[i].len);
        CHECK_MSG(s_render_calls == 0 && s_tx_calls == 0, "越界（%s）必须静默丢弃（render=%d tx=%d）",
                  bad[i].why, s_render_calls, s_tx_calls);
    }
}

/** @brief A3 全屏填充：颜色映射 / 覆盖整块逻辑屏 / 不持久化 / 越界拒绝
 *
 *  **两条断言已在 2026-10-19 补齐**：`data_len != 1` 的拒收（作者补）与
 *  `0x34..0x37`（蓝/紫/青/白）**全彩全放行**的口径（协议方确认支持全彩）。 */
static void case_cmd_fill_screen(void)
{
    TEST_BEGIN("A3 全屏填充：颜色映射 / 逻辑屏满幅 / 不持久化 / 越界拒绝");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};

    /* 0x31 红 → 一次 FILL，覆盖整块逻辑屏（224×100），且**不得**带 persist */
    uint8_t v = 0x31;
    tx_reset();
    render_reset();
    _cdstd_fill_screen(&chan, &v, 1);
    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB3 && s_tx_buf[9] == 0x31,
              "0x31 应回 0xB3/0x31（cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_FILL && s_rc_x == 0 &&
                  s_rc_y == 0 && s_rc_w == TEST_LOGICAL_W && s_rc_h == TEST_LOGICAL_H &&
                  s_rc_color == DEV_DISPLAY_COLOR_RED && s_rc_persist == false,
              "应是 FILL(0,0,224,100,RED) 且 persist=false（calls=%d w=%u h=%u color=%u persist=%d）",
              s_render_calls, (unsigned)s_rc_w, (unsigned)s_rc_h, (unsigned)s_rc_color,
              (int)s_rc_persist);

    /* 0x30 黑（清屏语义）→ 同样满幅 + 不持久化 */
    v = 0x30;
    tx_reset();
    render_reset();
    _cdstd_fill_screen(&chan, &v, 1);
    CHECK_MSG(s_tx_buf[8] == 0xB3 && s_tx_buf[9] == 0x31, "0x30 应回 0xB3/0x31");
    CHECK_MSG(s_render_calls == 1 && s_rc_color == DEV_DISPLAY_COLOR_BLACK && s_rc_persist == false,
              "0x30 应是 FILL(BLACK) 且 persist=false");

    /* 全彩：0x36 青 → 同样满幅 + 不持久化（0x34..0x37 都已放行） */
    v = 0x36;
    tx_reset();
    render_reset();
    _cdstd_fill_screen(&chan, &v, 1);
    CHECK_MSG(s_tx_buf[8] == 0xB3 && s_tx_buf[9] == 0x31, "0x36 应回 0xB3/0x31");
    CHECK_MSG(s_render_calls == 1 && s_rc_color == DEV_DISPLAY_COLOR_CYAN && s_rc_persist == false,
              "0x36 应映射为青（全彩）且 persist=false");

    /* 越界：0x29（低于 0x30，按无符号回绕后被上界判断拦下）、0x38（高于文档上限 0x33）、
       0xFF → 回 0xB3/0x30，且**一次都不许调 render** */
    static const uint8_t bad[] = {0x29, 0x38, 0xFF};
    for (unsigned i = 0; i < sizeof(bad); i++) {
        tx_reset();
        render_reset();
        uint8_t b = bad[i];
        _cdstd_fill_screen(&chan, &b, 1);
        CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB3 && s_tx_buf[9] == 0x30,
                  "颜色 %02X 应回 0xB3/0x30（cmd=%02X status=%02X）", bad[i], s_tx_buf[8],
                  s_tx_buf[9]);
        CHECK_MSG(s_render_calls == 0, "颜色 %02X 越界时不得调 render（calls=%d）", bad[i],
                  s_render_calls);
    }
}

/** @brief 上电应用：记录有效才改网络；无效不动；端口 0 不覆盖监听口 */
static void case_boot_apply(void)
{
    TEST_BEGIN("上电应用：有效才改网络；无效不动；端口 0 不覆盖");

    net_stub_reset();

    /* 1) 无有效配置 → 一个字节都不碰 */
    s_fake_cfg_valid = false;
    _cdstd_apply_net_cfg();
    CHECK_MSG(s_net_set_calls == 0 && s_udp_port_calls == 0,
              "无有效配置时不得改网络（set_ip %d 次 / port %d 次）", s_net_set_calls,
              s_udp_port_calls);

    /* 2) 有效配置 → IP/掩码/网关原样应用；端口 大端字节对 → 数值（0x27/0x2C → 0x272C） */
    s_fake_cfg_valid = true;
    s_fake_cfg       = (app_cdstd_net_cfg_t){.ip   = {192, 168, 0, 220},
                                             .mask = {255, 255, 255, 0},
                                             .gw   = {192, 168, 0, 1},
                                             .port = {0x27, 0x2C}};
    _cdstd_apply_net_cfg();
    CHECK_MSG(s_net_set_calls == 1 && memcmp(s_net_ip, s_fake_cfg.ip, 4) == 0 &&
                  memcmp(s_net_mask, s_fake_cfg.mask, 4) == 0 &&
                  memcmp(s_net_gw, s_fake_cfg.gw, 4) == 0,
              "有效配置应原样应用到 pl_net（调用 %d 次）", s_net_set_calls);
    CHECK_MSG(s_udp_port_calls == 1 && s_udp_port_set == 0x272C,
              "端口应按大端字节对解码后应用（应 0x272C，得到 %d 次/%04X）", s_udp_port_calls,
              (unsigned)s_udp_port_set);

    /* 3) 配置里端口为 0 → 只改 IP，不把监听口设成 0（那是随机端口） */
    s_fake_cfg.port[0] = 0;
    s_fake_cfg.port[1] = 0;
    _cdstd_apply_net_cfg();
    CHECK_MSG(s_net_set_calls == 2 && s_udp_port_calls == 1,
              "端口 0 不应覆盖监听口（set %d / port %d）", s_net_set_calls, s_udp_port_calls);
}

/** @brief 0xA5 设置 IP：长度校验 / 落盘入参 / IAP 镜像端口解码 / 状态字节与应答 CRC */
static void case_cmd_set_ip(void)
{
    TEST_BEGIN("0xA5 设置 IP：长度校验、保存与 IAP 镜像、状态字节与应答 CRC");

    static app_ccb_t    chan = {.name = "cdstd_cmd_ut"};
    app_cdstd_net_cfg_t net  = {.ip   = {192, 168, 0, 220},
                                .mask = {255, 255, 255, 0},
                                .gw   = {192, 168, 0, 1},
                                .port = {0x27, 0x2C}};

    /* 1) 长度不对 → 回 0xB5/0x30，且不落盘、不镜像 */
    tx_reset();
    cfg_stub_reset();
    _cdstd_set_ip(&chan, &net, (uint16_t)(sizeof(net) - 1));
    CHECK_MSG(s_tx_calls == 1 && s_tx_len == sizeof(app_cdstd_reply_status_t) &&
                  s_tx_buf[8] == 0xB5 && s_tx_buf[9] == 0x30,
              "长度不对应回 0xB5/0x30（cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_save_calls == 0 && s_iap_calls == 0, "长度不对不得落盘/镜像");

    /* 2) 正常 → 落盘入参 = 那 14 字节；IAP 镜像端口 = 解码后的 0x272C；回 0x31 */
    tx_reset();
    cfg_stub_reset();
    _cdstd_set_ip(&chan, &net, (uint16_t)sizeof(net));
    CHECK_MSG(s_tx_calls == 1 && s_tx_buf[8] == 0xB5 && s_tx_buf[9] == 0x31, "正常应回 0xB5/0x31");
    CHECK_MSG(s_save_calls == 1 && memcmp(s_save_buf, &net, sizeof(net)) == 0,
              "保存入参应为帧内那 14 字节");
    CHECK_MSG(s_iap_calls == 1 && memcmp(s_iap_ip, net.ip, 4) == 0 &&
                  memcmp(s_iap_mask, net.mask, 4) == 0 && memcmp(s_iap_gw, net.gw, 4) == 0 &&
                  s_iap_port == 0x272C,
              "IAP 镜像应带解码后的端口 0x272C（得到 %u）", (unsigned)s_iap_port);

    /* 应答自洽：CRC 覆盖 VER~DATA、大端落在尾部 —— 用用例自己的 CRC 复算 */
    uint16_t crc = crc16_xmodem(&s_tx_buf[2], (size_t)(s_tx_len - 4));
    CHECK_MSG(s_tx_buf[s_tx_len - 2] == (uint8_t)(crc >> 8) &&
                  s_tx_buf[s_tx_len - 1] == (uint8_t)crc,
              "应答 CRC 应为 VER~DATA 的 crc16（大端）");

    /* 3) 落盘失败 → 回 0x30，但 IAP 镜像照做（与 LDI 同语义，test_ldi_0ah 钉着） */
    tx_reset();
    cfg_stub_reset();
    s_save_ret = -1;
    _cdstd_set_ip(&chan, &net, (uint16_t)sizeof(net));
    CHECK_MSG(s_tx_buf[8] == 0xB5 && s_tx_buf[9] == 0x30, "保存失败应回 0x30");
    CHECK_MSG(s_iap_calls == 1, "保存失败也要更新 IAP 镜像（既有语义）");
    s_save_ret = 0;
}

/** @brief 0xA6 上报 IP：记录优先；记录无效回落运行态（端口同样转大端字节对） */
static void case_cmd_report_ip(void)
{
    TEST_BEGIN("0xA6 上报 IP：记录优先；无效回落运行态");

    static app_ccb_t chan = {.name = "cdstd_cmd_ut"};

    /* 1) 记录有效 → 广播 B6，带记录里的 14 字节（LEN=0x0F） */
    tx_reset();
    s_fake_cfg_valid = true;
    s_fake_cfg       = (app_cdstd_net_cfg_t){.ip   = {192, 168, 0, 220},
                                             .mask = {255, 255, 255, 0},
                                             .gw   = {192, 168, 0, 1},
                                             .port = {0x27, 0x2C}};
    _cdstd_report_ip(&chan, nullptr, 0);
    CHECK_MSG(s_tx_to_calls == 1 && s_tx_len == sizeof(app_cdstd_reply_ip_t),
              "应广播一条 B6（%d 次 / %u 字节）", s_tx_to_calls, (unsigned)s_tx_len);
    CHECK_MSG(s_tx_buf[6] == 0x00 && s_tx_buf[7] == 0x0F && s_tx_buf[8] == 0xB6,
              "B6 的 LEN/CMD 应对（LEN=0x0F, cmd=%02X）", s_tx_buf[8]);
    CHECK_MSG(memcmp(&s_tx_buf[9], &s_fake_cfg, sizeof(s_fake_cfg)) == 0,
              "B6 的 14 字节应等于记录内容");
    uint16_t crc = crc16_xmodem(&s_tx_buf[2], (size_t)(s_tx_len - 4));
    CHECK_MSG(s_tx_buf[s_tx_len - 2] == (uint8_t)(crc >> 8) &&
                  s_tx_buf[s_tx_len - 1] == (uint8_t)crc,
              "B6 的 CRC 应为 VER~DATA 的 crc16（大端）");

    /* 2) 记录无效 → 回落运行态（IP 取 pl_net_get_ip、端口取 UDP 通道口并转大端） */
    tx_reset();
    s_fake_cfg_valid = false;
    s_rt_port        = 10011; /* 0x271B：与上一步的记录值不同，证明真的走了回落 */
    _cdstd_report_ip(&chan, nullptr, 0);
    CHECK_MSG(s_tx_to_calls == 1 && s_tx_len == sizeof(app_cdstd_reply_ip_t),
              "无效记录也要回 B6（%d 次 / %u 字节）", s_tx_to_calls, (unsigned)s_tx_len);
    CHECK_MSG(memcmp(&s_tx_buf[9], s_rt_ip, 4) == 0 && memcmp(&s_tx_buf[13], s_rt_mask, 4) == 0 &&
                  memcmp(&s_tx_buf[17], s_rt_gw, 4) == 0,
              "B6 应回落运行态 IP/掩码/网关");
    CHECK_MSG(s_tx_buf[21] == 0x27 && s_tx_buf[22] == 0x1B, "运行态端口应转大端字节对（0x271B）");
}

/* ================================================================
 *  任务侧：取帧与分派（命令层贯通）
 * ================================================================ */

/** @brief 任务贯通：空队列不误取、真帧经真命令表分派、越界命令丢弃
 *
 *  回归背景：队列取帧判断曾写反（超时分支拿未初始化的 msg 去查表），现场表现是
 *  "收到点噪声就按命令 0 处理"。这里用真任务 + 真命令表 + 真处理函数把整条链路
 *  走通，并钉住"越界 aux 直接丢、不产生任何应答"。 */
static void case_task_dispatch(void)
{
    TEST_BEGIN("任务：空队列不误取、真帧分派（含命令层贯通）、越界命令丢弃");

    /* 上电应用的两条路径已由 case_boot_apply 直接覆盖；让 init 内那次应用走"无配置"分支 */
    s_fake_cfg_valid = false;

    _cdstd_module_init();
    CHECK_MSG(s_bind_calls == 2, "应绑定 udp/rs485 两个通道（得到 %d）", s_bind_calls);
    CHECK_MSG(s_cdstd_msg_queue != nullptr, "帧队列应已建立");

    /* 空队列：任务反复超时也不能拿未初始化的 msg 去查表
       **反向验证**：把取帧判断反过来（osOK == 时 continue），超时分支会拿零初始化的 msg
       去查表，下面这条"不得有任何应答"立刻红。 */
    tx_reset();
    usleep(400 * 1000);
    CHECK_MSG(s_tx_calls + s_tx_to_calls == 0, "空队列时不应发出任何应答（得到 %d 次）",
              s_tx_calls + s_tx_to_calls);

    static uint8_t msg_buf[CDSTD_MSG_SIZE] __attribute__((aligned(4)));
    app_dispatch_msg_t *m   = (app_dispatch_msg_t *)msg_buf;
    static app_ccb_t    s_chan = {.name = "cdstd_ut_chan"};
    uint8_t             frame[128];

    /* 1) aux=3（0xA3 全屏）→ 贯通到底：任务取帧 → 真表 → 真处理函数 → FILL(RED) + 回 0xB3/0x31
       （DATA 必须给合法颜色：A3 的处理函数会校验，全零的 dummy 会被判 0x30） */
    const uint8_t fill_red = 0x31;
    uint16_t      n        = cdstd_build(frame, 0xA3, 1, &fill_red);
    m->ccb                 = &s_chan;
    m->aux                 = 3;
    memcpy(m->data, frame, n);
    tx_reset();
    render_reset();
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    CHECK_MSG(wait_tx(1, 2000), "投递的帧未被分派（取帧判断或表查询仍有问题）");
    CHECK_MSG(s_tx_len == sizeof(app_cdstd_reply_status_t) && s_tx_buf[8] == 0xB3 &&
                  s_tx_buf[9] == 0x31,
              "aux=3 应回 0xB3/0x31（得到 cmd=%02X status=%02X）", s_tx_buf[8], s_tx_buf[9]);
    CHECK_MSG(s_render_calls == 1 && s_rc_type == APP_RENDER_TYPE_FILL &&
                  s_rc_color == DEV_DISPLAY_COLOR_RED && s_rc_w == TEST_LOGICAL_W &&
                  s_rc_persist == false,
              "aux=3 的帧应被渲染成 FILL(RED, 整屏)（calls=%d color=%u）", s_render_calls,
              (unsigned)s_rc_color);

    /* 2) 越界 aux（>= CDSTD_CMD_COUNT）→ 直接丢，不产生任何应答 */
    m->aux = CDSTD_CMD_COUNT;
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    usleep(200 * 1000);
    CHECK_MSG(s_tx_calls + s_tx_to_calls == 1, "越界命令不应产生应答（总数 %d）",
              s_tx_calls + s_tx_to_calls);

    /* 3) aux=5（0xA5 设置 IP）→ 贯通到保存与 IAP 镜像：钉住"DATA 域指针 + LEN-1"的传参 */
    static const app_cdstd_net_cfg_t net_in = {.ip   = {192, 168, 0, 220},
                                               .mask = {255, 255, 255, 0},
                                               .gw   = {192, 168, 0, 1},
                                               .port = {0x27, 0x2C}};
    n = cdstd_build(frame, 0xA5, (uint16_t)sizeof(net_in), &net_in);
    m->aux = 5;
    memcpy(m->data, frame, n);
    tx_reset();
    cfg_stub_reset();
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    CHECK_MSG(wait_tx(1, 2000), "设置 IP 的帧未被分派");
    CHECK_MSG(s_save_calls == 1 && memcmp(s_save_buf, &net_in, sizeof(net_in)) == 0,
              "任务应把帧内 DATA 域（14 字节）原样交给保存");
    CHECK_MSG(s_iap_calls == 1 && s_iap_port == 0x272C, "IAP 镜像应收到解码后的端口");
    CHECK_MSG(s_tx_buf[8] == 0xB5 && s_tx_buf[9] == 0x31, "0xA5 应回 0xB5/0x31");
}

/* ================================================================ */

int main(void)
{
    printf("CDSTD 协议 host 单测\n");

    case_probe_states();
    case_wait_whole_frame();
    case_length_domain();
    case_crc_fail_is_fake();
    case_scratch_contract();
    case_rb_capacity_contract();
    case_fake_must_not_copy_whole();
    case_no_cross_poison();
    case_split_across_rb_wrap();
    case_cmd_word_display();
    case_cmd_word_display_bounds();
    case_cmd_word_display_endian();
    case_cmd_screen_switch();
    case_cmd_screen_switch_bounds();
    case_cmd_light_adjust();
    case_cmd_light_adjust_bounds();
    case_cmd_persistent_display();
    case_cmd_picture_display();
    case_cmd_picture_display_bounds();
    case_cmd_release_display();
    case_cmd_release_display_bounds();
    case_cmd_fill_screen();
    case_boot_apply();
    case_cmd_set_ip();
    case_cmd_report_ip();
    case_task_dispatch();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
