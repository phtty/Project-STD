/**
 * @file    test_lcsp.c
 * @brief   LCSP（外贸车道屏以太网协议）host 单测：组帧与协议附录 A 的字节级对拍、
 *          探针四态（WAIT / FAKE / SKIP / READY）与长度域、跨协议不毒化、
 *          任务分派与**重复帧原样重发**、未知命令 → 0xF0；
 *          命令层 0x01 设备信息（15 字节 RDATA：序列号 / 几何 / 槽位数 / 亮度 / 状态，含附录 A.17 对拍）/
 *          0x02 设置 IP / 0x03 查询 IP / 0x04 全屏填充 /
 *          0x05 显示图片 / 0x06 显示已存图片 / 0x07 亮度 / 0x08 显示开关 / 0x09 重启；
 *          模块上电应用配置（端口 / IP / 亮度 / 屏开关）
 *
 * 写法契约见 docs/测试约定.md（桩、越界矩阵、断言预算、登记与验证清单）。
 *
 * **为什么需要这个测试**：本协议与工程内其它协议有三处**静默**差异，错了都不报错：
 *   1. 应答命令字 = 请求码高低半字节互换（`0x03` ↔ `0x30`）。漏一条就是"设备照做了但
 *      没回"，上位机只能按超时重传 —— case_build_frame_golden 用协议附录 A 的原始
 *      字节对拍，连 CRC 的**低字节在前**一起钉住；
 *   2. SEQ 必须原样回显（协议 4.3.2）。错一位上位机就配不上对，把应答当噪声；
 *   3. CRC 覆盖 SEQ 起至 DATA 末、reflected 0x8408。种子哨兵 0x906E 单独成 case，
 *      免得"CRC 覆盖范围"与"组帧逻辑"两处错得刚好互相抵消。
 *
 * 另外三处**现场才暴露**的行为各有一组用例：
 *   · 探针在整帧到齐之前比 CRC → 大帧（0x05 整屏图）分段到达时永远被当坏帧丢，
 *     症状是"显示图片不生效、小命令都正常"（case_probe_partial_frame）；
 *   · CRC 不过却返回 SKIP → 框架按伪长度整段跳过，把同一批里的后续真帧一起啃掉
 *     （case_probe_crc_fail）；
 *   · 重复帧不判重 → 一幅图重存一次槽位（一次 64KB 扇区擦写）、重启命令被重发就重启
 *     两次；只丢不答又会让上位机判链路故障（case_task_dup_frame）。
 *
 * 手法：用例 TU-include 生产 TU（探针/组帧/任务/队列与判重缓存都是 static，外部够不着）；
 * 命令层 app_lcsp_cmd.c 作为**独立 TU 编译并跑真的** —— 命令表、位序翻转、参数校验都是
 * 被测对象，用例只经 `g_lcsp_cmd_table[]` 调用（不复制、不改写这张表）。
 * 外部依赖（配置记录 / 槽位记录 / 渲染 / 整屏几何 / 网络 / 通道 / 复位）全部是记录桩，
 * 桩只记录不下结论。
 *
 * 帧由用例**独立构造**（`frame_make` 自己写帧头/长度/CRC 落位，不复用 `app_lcsp_build_frame`），
 * 这样组帧与探针是两条独立路径；两者共用 `crc16_xmodem` 是刻意的 —— 那个算法本身由
 * 协议附录 A 的哨兵字节钉住。
 */

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_dispatch.h"
#include "app_iap_cfg.h"
#include "app_light_sensor.h"
#include "app_lcsp.h"
#include "app_lcsp_cfg.h"
#include "app_lcsp_cmd.h"
#include "app_lcsp_slot.h"
#include "app_render.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "app_udp.h"
#include "cmsis_os2.h"
#include "crc_utils.h"
#include "pl_net.h"
#include "pl_sys.h"
#include "ring_buffer.h"

/* ================================================================
 *  桩：通道注册与访问器
 *
 *  app_lcsp.c 的模块 init 会把 pcb 绑到 TCP/UDP 两个通道；这里只记录绑定次数。
 *  通道对象给真实地址（不是 nullptr）—— `_lcsp_send_reply` 对 ccb 有空守卫，
 *  给 nullptr 会把"应答没发出去"掩盖成"发送路径没跑"。
 * ================================================================ */

static app_ccb_t s_tcp_ccb_obj = {.name = "lcsp_ut_tcp"};
static app_ccb_t s_udp_ccb_obj = {.name = "lcsp_ut_udp"};

static int s_bind_calls;

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
    s_bind_calls++;
}

app_ccb_t *app_tcp_server_ccb(void) { return &s_tcp_ccb_obj; }
app_ccb_t *app_udp_ccb(void) { return &s_udp_ccb_obj; }

/** @brief 复位绑定记录（模块 init 会绑两条通道；用例开头清一遍） */
static void bind_stub_reset(void) { s_bind_calls = 0; }

/* ================================================================
 *  桩：发送记录（应答唯一可观测的出口）
 *
 *  `app_ccb_send_to` 是任务发应答的唯一路径，记录**字节、长度、广播标志、通道**：
 *  · 广播标志是 0x03 的协议行为（协议 7.3：应答走 UDP 广播，单播在跨网段时回不去）；
 *  · 字节原样留下来给"重复帧要回同样的应答"逐字节对拍。
 * ================================================================ */

static int             s_tx_calls;
static int             s_tx_len;
static bool            s_tx_broadcast;
static const app_ccb_t *s_tx_ccb;
static uint8_t         s_tx_buf[LCSP_REPLY_MAX_LEN];
static int             s_event_seq; /**< 全局单调事件序号（发送 / 复位共用，判先后）*/
static int             s_tx_event;

int32_t app_ccb_send_to(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    s_tx_calls++;
    s_tx_len       = (int)len;
    s_tx_broadcast = (nullptr != dst) && dst->broadcast;
    s_tx_ccb       = ccb;
    s_tx_event     = ++s_event_seq;
    if (len <= (uint16_t)sizeof(s_tx_buf))
        memcpy(s_tx_buf, data, len);
    return 0;
}

int32_t app_ccb_send(app_ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    static const app_ccb_dst_t local = {.broadcast = false};

    return app_ccb_send_to(ccb, &local, data, len);
}

/** @brief 复位发送记录（用例开头调用；计数与字节一起清，免得上一条用例的残留值
 *         把断言失败现场的排查指向错误的调用） */
static void tx_reset(void)
{
    s_tx_calls     = 0;
    s_tx_len       = 0;
    s_tx_broadcast = false;
    s_tx_ccb       = nullptr;
    s_tx_event     = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));
}

/* ================================================================
 *  桩：复位（必须"记一笔就返回"）
 *
 *  0x09 的验收点就是"应答先发出、再复位"（协议 7.9.1）。真复位在 host 上等于杀进程，
 *  所以这里只记次数与**时间序号**：用例比较 `reset_event > tx_event` 就能证明顺序。
 * ================================================================ */

static int s_reset_calls;
static int s_reset_event;

void pl_system_reset(void)
{
    s_reset_calls++;
    s_reset_event = ++s_event_seq;
}

static void reset_stub_reset(void)
{
    s_reset_calls = 0;
    s_reset_event = 0;
}

/* ================================================================
 *  桩：LCSP 配置记录（网络 + 序列号 + 亮度 + 屏开关）
 *
 *  · `s_cfg` 扮演 Flash 里的记录：save 写它、load 读它；
 *  · `s_cfg_readback_override` 让"落盘后的读回值"与请求不同 —— 0x02 的应答必须
 *    取自**读回**（协议 7.2：回显只能证明"收到了"，读回才证明"落盘了"），
 *    两边一样的话这条断言就恒真、测不出实现走的是哪一条。
 * ================================================================ */

static bool            s_cfg_valid;
static app_lcsp_cfg_t  s_cfg;        /**< "Flash 里的记录" */
static app_lcsp_cfg_t  s_cfg_saved;  /**< 最近一次 save 的**入参**（原样，不被读回改写）*/
static int             s_cfg_save_calls;
static int32_t         s_cfg_save_ret;
static bool            s_cfg_readback_override;
static uint8_t         s_cfg_readback_port[2];

bool app_lcsp_cfg_load(app_lcsp_cfg_t *cfg)
{
    if (!s_cfg_valid) {
        memset(cfg, 0, sizeof(*cfg));
        return false;
    }
    *cfg = s_cfg;
    return true;
}

int32_t app_lcsp_cfg_save(const app_lcsp_cfg_t *cfg)
{
    s_cfg_save_calls++;
    s_cfg_saved = *cfg;
    if (0 != s_cfg_save_ret)
        return s_cfg_save_ret;

    s_cfg       = *cfg;
    s_cfg_valid = true;
    if (s_cfg_readback_override) {
        s_cfg.port[0] = s_cfg_readback_port[0];
        s_cfg.port[1] = s_cfg_readback_port[1];
    }
    return 0;
}

void app_lcsp_cfg_default(app_lcsp_cfg_t *cfg)
{
    static const uint8_t ip[4]   = {192, 168, 1, 100};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4]   = {192, 168, 1, 1};

    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->ip, ip, 4);
    memcpy(cfg->mask, mask, 4);
    memcpy(cfg->gw, gw, 4);
    cfg->port[0]    = (uint8_t)(LCSP_PORT_DEFAULT >> 8);
    cfg->port[1]    = (uint8_t)LCSP_PORT_DEFAULT;
    memcpy(cfg->serial, LCSP_SERIAL_DEFAULT, LCSP_SERIAL_LEN);
    cfg->brightness = LCSP_BRIGHTNESS_DEFAULT;
    cfg->oe_enabled = 1U;
}

/** @brief 复位配置记录桩：`s_cfg_save_ret` 带"失败注入"语义，残留到下一条用例会把
 *         正常路径变成失败路径，所以一并归零 */
static void cfg_stub_reset(void)
{
    s_cfg_valid            = false;
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_cfg_saved, 0, sizeof(s_cfg_saved));
    s_cfg_save_calls       = 0;
    s_cfg_save_ret         = 0;
    s_cfg_readback_override = false;
    s_cfg_readback_port[0] = 0;
    s_cfg_readback_port[1] = 0;
}

/* ================================================================
 *  桩：LCSP 槽位记录（3 个断电保存槽）
 *
 *  只做"记录是否完整"的模拟：槽位号越界 / 记录不存在 / 装不下 → false。
 *  几何是否配得上当前屏**不在槽位层判**（与生产实现的分工一致），那是命令层的事。
 * ================================================================ */

typedef struct {
    bool     valid;
    uint16_t rows;
    uint16_t cols;
    uint8_t  color;
    uint16_t bm_len;
    uint8_t  bm[LCSP_BITMAP_MAX_BYTES];
} slot_rec_t;

static slot_rec_t s_slots[LCSP_SLOT_MAX + 1U]; /**< 下标即槽位号；0 恒不用 */
static int        s_slot_save_calls;
static int32_t    s_slot_save_ret;
static uint8_t    s_slot_save_slot;
static uint16_t   s_slot_save_len;

bool app_lcsp_slot_is_occupied(uint8_t slot)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX))
        return false;
    return s_slots[slot].valid;
}

bool app_lcsp_slot_any_occupied(void)
{
    for (uint8_t i = 1U; i <= LCSP_SLOT_MAX; i++) {
        if (s_slots[i].valid)
            return true;
    }
    return false;
}

int32_t app_lcsp_slot_save(uint8_t slot, const app_lcsp_slot_image_t *img, uint16_t bitmap_len)
{
    s_slot_save_calls++;
    s_slot_save_slot = slot;
    s_slot_save_len  = bitmap_len;

    if (0 != s_slot_save_ret)
        return s_slot_save_ret;
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX))
        return -1;
    if (bitmap_len > (uint16_t)sizeof(s_slots[slot].bm))
        return -1;

    s_slots[slot].valid   = true;
    s_slots[slot].rows    = img->rows;
    s_slots[slot].cols    = img->cols;
    s_slots[slot].color   = img->color;
    s_slots[slot].bm_len  = bitmap_len;
    memcpy(s_slots[slot].bm, img->bitmap, bitmap_len);
    return 0;
}

bool app_lcsp_slot_load(uint8_t slot, app_lcsp_slot_image_t *img, uint16_t img_cap, uint16_t *bitmap_len)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX) || !s_slots[slot].valid)
        return false;

    uint32_t need = (uint32_t)sizeof(app_lcsp_slot_image_t) + (uint32_t)s_slots[slot].bm_len;
    if (need > (uint32_t)img_cap)
        return false;

    img->rows  = s_slots[slot].rows;
    img->cols  = s_slots[slot].cols;
    img->color = s_slots[slot].color;
    memcpy(img->bitmap, s_slots[slot].bm, s_slots[slot].bm_len);
    *bitmap_len = s_slots[slot].bm_len;
    return true;
}

/** @brief 往槽位里放一幅"已存好的图"（0x06 的输入）；slot 越界时什么都不做 */
static void slot_seed(uint8_t slot, uint16_t rows, uint16_t cols, uint8_t color, const uint8_t *bm,
                      uint16_t len)
{
    if ((slot < 1U) || (slot > LCSP_SLOT_MAX))
        return;

    s_slots[slot].valid  = true;
    s_slots[slot].rows   = rows;
    s_slots[slot].cols   = cols;
    s_slots[slot].color  = color;
    s_slots[slot].bm_len = (len > (uint16_t)sizeof(s_slots[slot].bm))
                               ? (uint16_t)sizeof(s_slots[slot].bm)
                               : len;
    memset(s_slots[slot].bm, 0, sizeof(s_slots[slot].bm));
    memcpy(s_slots[slot].bm, bm, s_slots[slot].bm_len);
}

static void slot_stub_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slot_save_calls = 0;
    s_slot_save_ret   = 0;
    s_slot_save_slot  = 0;
    s_slot_save_len   = 0;
}

/* ================================================================
 *  桩：整屏几何 / 渲染 / 亮度 / 屏开关 / 光敏
 *
 *  几何用**很小的假屏 8×2**（1bpp 位图 = ceil(8/8)×2 = 2 字节）：本套件断言的是
 *  "映射与校验"，不是某块板有多大 —— 用真几何会让每条用例都搬 2800 字节。
 *  另有一条用例把几何放大到 232×100（位图 2900 > LCSP_BITMAP_MAX_BYTES）来钉"超上限
 *  必须明确拒绝、不猜"。
 *
 *  `app_render` 桩把位图**拷一份**再记：源指针指向命令层那块 static 工作缓冲，
 *  下一次调用会被覆盖 —— 只记指针的话"位序翻转"这条断言会在后续调用后失效。
 * ================================================================ */

static uint16_t s_screen_rows = 8U;
static uint16_t s_screen_cols = 2U;

uint16_t app_screen_rows(void) { return s_screen_rows; }
uint16_t app_screen_cols(void) { return s_screen_cols; }

static int                 s_render_calls;
static app_render_type_t   s_rc_type;
static uint16_t            s_rc_x;
static uint16_t            s_rc_y;
static uint16_t            s_rc_w;
static uint16_t            s_rc_h;
static dev_display_color_t s_rc_color;
static bool                s_rc_persist;
static const uint8_t      *s_rc_bitmap;
static uint16_t            s_rc_bitmap_len;
static uint8_t             s_rc_bitmap_buf[LCSP_BITMAP_MAX_BYTES];

/* 第一次调用的现场：0x05/0x06 现在是"先清整屏、再贴图"两次调用，
   只记最后一次的 s_rc_* 看不到那条清屏。 */
static app_render_type_t   s_rc0_type;
static uint16_t            s_rc0_x;
static uint16_t            s_rc0_y;
static uint16_t            s_rc0_w;
static uint16_t            s_rc0_h;
static dev_display_color_t s_rc0_color;

void app_render(const app_render_cfg_t *cfg)
{
    if (0 == s_render_calls) {
        s_rc0_type  = cfg->type;
        s_rc0_x     = cfg->x;
        s_rc0_y     = cfg->y;
        s_rc0_w     = cfg->w;
        s_rc0_h     = cfg->h;
        s_rc0_color = cfg->color;
    }
    s_render_calls++;
    s_rc_type    = cfg->type;
    s_rc_x       = cfg->x;
    s_rc_y       = cfg->y;
    s_rc_w       = cfg->w;
    s_rc_h       = cfg->h;
    s_rc_color   = cfg->color;
    s_rc_persist = cfg->persist;

    /* union 只按 type 读（docs/测试约定.md §4.1-5）：非 BITMAP 时读 bitmap 是别的成员 */
    if (APP_RENDER_TYPE_BITMAP != cfg->type)
        return;

    s_rc_bitmap = cfg->bitmap;
    uint32_t n  = ((uint32_t)(cfg->w + 7U) / 8U) * (uint32_t)cfg->h;
    if (n > sizeof(s_rc_bitmap_buf))
        n = sizeof(s_rc_bitmap_buf);
    s_rc_bitmap_len = (uint16_t)n;
    memset(s_rc_bitmap_buf, 0, sizeof(s_rc_bitmap_buf));
    if ((nullptr != cfg->bitmap) && (n != 0U))
        memcpy(s_rc_bitmap_buf, cfg->bitmap, n);
}

static void render_reset(void)
{
    s_render_calls  = 0;
    s_rc_type       = (app_render_type_t)0;
    s_rc_x = s_rc_y = s_rc_w = s_rc_h = 0;
    s_rc_color      = DEV_DISPLAY_COLOR_BLACK;
    s_rc_persist    = false;
    s_rc_bitmap     = nullptr;
    s_rc_bitmap_len = 0;
    memset(s_rc_bitmap_buf, 0, sizeof(s_rc_bitmap_buf));
    s_rc0_type  = (app_render_type_t)0;
    s_rc0_x = s_rc0_y = s_rc0_w = s_rc0_h = 0;
    s_rc0_color = DEV_DISPLAY_COLOR_BLACK;
}

static bool    s_oe_state = true;
static uint8_t s_bright_hw = 7U;
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

void app_light_sensor_resume(void) { s_resume_calls++; }

/** @brief 复位几何/亮度/屏开关桩（几何回到 8×2、亮度硬件值 7、屏开） */
static void screen_stub_reset(void)
{
    s_screen_rows      = 8U;
    s_screen_cols      = 2U;
    s_oe_state         = true;
    s_bright_hw        = 7U;
    s_fixed_calls      = 0;
    s_fixed_level      = 0;
    s_resume_calls     = 0;
    s_oe_enable_calls  = 0;
    s_oe_disable_calls = 0;
}

/* ================================================================
 *  桩：网络（运行态 IP / 端口）与 IAP 记录镜像
 *
 *  运行态与"配置记录"是两个东西（协议 7.3.2 要求报运行态）：0x03 必须读
 *  `pl_net_get_ip` + `app_tcp_server_get_port`，不是读 Flash 记录。
 * ================================================================ */

static uint8_t  s_net_ip[4];
static uint8_t  s_net_mask[4];
static uint8_t  s_net_gw[4];
static uint16_t s_tcp_port = 10028U;

static int      s_net_set_calls;
static int      s_tcp_port_calls;
static uint16_t s_tcp_port_set;
static int      s_udp_port_calls;
static uint16_t s_udp_port_set;

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    s_net_set_calls++;
    memcpy(s_net_ip, ip, 4);
    memcpy(s_net_mask, mask, 4);
    memcpy(s_net_gw, gw, 4);
}

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_net_ip, 4);
    memcpy(mask, s_net_mask, 4);
    memcpy(gw, s_net_gw, 4);
}

void app_tcp_server_set_port(uint16_t port)
{
    s_tcp_port_calls++;
    s_tcp_port_set = port;
    s_tcp_port     = port;
}

uint16_t app_tcp_server_get_port(void) { return s_tcp_port; }

void app_udp_set_port(uint16_t port)
{
    s_udp_port_calls++;
    s_udp_port_set = port;
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

/** @brief 复位网络与 IAP 镜像桩（运行态一并归零：残留的 IP 会让 0x03 的断言假绿） */
static void net_stub_reset(void)
{
    s_net_set_calls = 0;
    s_tcp_port_calls = 0;
    s_tcp_port_set   = 0;
    s_udp_port_calls = 0;
    s_udp_port_set   = 0;
    s_tcp_port       = LCSP_PORT_DEFAULT;
    memset(s_net_ip, 0, sizeof(s_net_ip));
    memset(s_net_mask, 0, sizeof(s_net_mask));
    memset(s_net_gw, 0, sizeof(s_net_gw));

    s_iap_calls = 0;
    s_iap_port  = 0;
    memset(s_iap_ip, 0, sizeof(s_iap_ip));
    memset(s_iap_mask, 0, sizeof(s_iap_mask));
    memset(s_iap_gw, 0, sizeof(s_iap_gw));
}

/** @brief 一次性复位**全部**记录桩（每个 `*_reset()` 都在这里被调用，用例开头统一清一遍）
 *
 *  逐条调用而不是 memset 一个结构：各桩的复位口径不同（如 `s_cfg_save_ret` 是失败注入位，
 *  不清会让下一条用例的"正常路径"断在"保存失败"上）。 */
static void stubs_reset(void)
{
    bind_stub_reset();
    tx_reset();
    reset_stub_reset();
    cfg_stub_reset();
    slot_stub_reset();
    render_reset();
    screen_stub_reset();
    net_stub_reset();
}

/* ================================================================ */
/*  被测：生产源码本体（探针 / 组帧 / 任务 / 判重缓存都是 static）   */
/* ================================================================ */

#include "../Application/Src/LCSP/app_lcsp.c"

/* ================================================================ */
/*  夹具                                                            */
/* ================================================================ */

/* 探针只用 self->rb；本套件自建 8KB 缓冲（最长帧 2811，还要塞"两段到达"的场景）。
   容量必须走 RB_DEFINE —— 裸 ring_buffer_t 手工赋值容易漏 size，取模除零。 */
RB_DEFINE(s_ut_rb, 8192);

static uint8_t s_frame[LCSP_FRAME_MAX_LEN];
static uint8_t s_scratch[FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));

static void fixture_reset(void)
{
    s_ut_rb.read_index  = 0;
    s_ut_rb.write_index = 0;
    s_lcsp_pcb.rb       = &s_ut_rb;
}

static void lcsp_feed(const uint8_t *d, uint16_t n) { rb_write(&s_ut_rb, d, n, nullptr); }

static app_pcb_probe_state_t lcsp_probe(uint32_t *total_len, uint8_t *aux, uint16_t scratch_cap)
{
    return app_lcsp_probe_frame(&s_lcsp_pcb, nullptr, nullptr, s_scratch, scratch_cap, total_len,
                                aux);
}

/**
 * @brief 独立组一条 LCSP 帧（**不复用** `app_lcsp_build_frame`）
 *
 * 帧头 `55 AA` | SEQ(2 大端) | CMD | LEN(2 大端) | DATA | CRC16(2 **低字节在前**)，
 * CRC 覆盖 SEQ 起的 5+LEN 字节。用例自己写落位，才能证明探针读的与协议一致 ——
 * 若两边共用组帧函数，CRC 覆盖范围写错会一起错、测试仍绿。
 */
static uint16_t frame_make(uint8_t *out, uint8_t cmd, uint16_t seq, const uint8_t *payload,
                           uint16_t plen)
{
    out[0] = 0x55;
    out[1] = 0xAA;
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)seq;
    out[4] = cmd;
    out[5] = (uint8_t)(plen >> 8);
    out[6] = (uint8_t)plen;
    if ((nullptr != payload) && (plen != 0U))
        memcpy(&out[7], payload, plen);

    uint16_t crc         = crc16_xmodem(&out[2], (size_t)(5U + plen));
    out[7 + plen]        = (uint8_t)(crc & 0xFFU);
    out[8 + plen]        = (uint8_t)(crc >> 8);
    return (uint16_t)(9U + plen);
}

/** @brief 复位判重缓存（任务侧 static）：用例之间不共享"上一条应答" */
static void dup_reset(void)
{
    s_last_reply_valid     = false;
    s_last_reply_len       = 0;
    s_last_reply_cmd       = 0;
    s_last_reply_broadcast = false;
    s_last_reply_ccb       = nullptr;
    memset(s_last_reply_seq, 0, sizeof(s_last_reply_seq));
    memset(s_last_reply, 0, sizeof(s_last_reply));
}

/* ---- 命令层直调：走**生产命令表** ---- */

static app_ccb_t      s_host_ccb = {.name = "lcsp_ut_host"};
static app_lcsp_req_t s_req;
static uint8_t        s_req_frame[LCSP_FRAME_MAX_LEN];
static uint8_t        s_reply[LCSP_REPLY_MAX_LEN];

/** @brief 用生产命令表跑一条请求（组包 / 校验 / CRC 全是真的），返回应答字节数
 *
 *  入口是 `g_lcsp_cmd_table[]` 而不是某个 static 处理函数：命令表本身也是被测对象
 *  （漏登记一行就是"照做了没回"，见 app_lcsp_cmd.h 文件头）。 */
static uint16_t cmd_run(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t plen)
{
    const uint8_t seq_bytes[2] = {(uint8_t)(seq >> 8), (uint8_t)seq};

    memset(s_reply, 0, sizeof(s_reply));
    (void)app_lcsp_build_frame(cmd, seq_bytes, payload, plen, s_req_frame,
                               (uint16_t)sizeof(s_req_frame));

    s_req = (app_lcsp_req_t){
        .ccb         = &s_host_ccb,
        .frame       = (const app_lcsp_frame_t *)s_req_frame,
        .payload     = ((const app_lcsp_frame_t *)s_req_frame)->data,
        .payload_len = plen,
        .reply_buf   = s_reply,
        .reply_cap   = (uint16_t)sizeof(s_reply),
    };

    const uint8_t fn = (uint8_t)(cmd & 0x0FU);
    if ((fn < APP_LCSP_CMD_COUNT) && (nullptr != g_lcsp_cmd_table[fn]))
        g_lcsp_cmd_table[fn](&s_req);

    return s_req.reply_len;
}

/** @brief 应答是否为"只有状态码"的帧：10 字节、LEN=1、状态在 DATA[0] */
static bool reply_is_status(uint8_t cmd, uint8_t status)
{
    return (s_req.reply_len == 10U) && (s_reply[4] == cmd) && (s_reply[5] == 0U) &&
           (s_reply[6] == 1U) && (s_reply[7] == status);
}

/** @brief 造 0x02/0x20 的 14 字节 DATA（ip / mask / gw / port，端口大端） */
static void ip_payload(uint8_t out[14], const uint8_t ip[4], const uint8_t mask[4],
                       const uint8_t gw[4], uint16_t port)
{
    memcpy(out, ip, 4);
    memcpy(out + 4, mask, 4);
    memcpy(out + 8, gw, 4);
    out[12] = (uint8_t)(port >> 8);
    out[13] = (uint8_t)port;
}

/* ---- 任务侧投递与等待 ---- */

static uint8_t   s_msg_buf[LCSP_MSG_SIZE] __attribute__((aligned(4)));
static app_ccb_t s_task_ccb = {.name = "lcsp_ut_task_chan"};

/** @brief 往协议队列投一帧（aux 由参数给，用来证明任务**不依赖** aux 分派） */
static bool task_feed(const uint8_t *frame, uint16_t n, uint8_t aux)
{
    app_dispatch_msg_t *m = (app_dispatch_msg_t *)s_msg_buf;

    memset(s_msg_buf, 0, sizeof(s_msg_buf));
    m->ccb  = &s_task_ccb;
    m->aux  = aux;
    m->data_len = n;
    memcpy(m->data, frame, n);

    return osMessageQueuePut(s_lcsp_msg_queue, m, 0, 0) == osOK;
}

static bool wait_tx(int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (s_tx_calls >= want)
            return true;
        usleep(5000);
    }
    return s_tx_calls >= want;
}

static bool wait_reset(int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (s_reset_calls >= want)
            return true;
        usleep(5000);
    }
    return s_reset_calls >= want;
}

/** @brief 等任务把"上一次应答"记进判重缓存
 *
 *  任务是在**发完应答之后**才写缓存的。重传帧若赶在写缓存之前投进去，判重会落空、业务
 *  被执行两次 —— 用例就成了看调度脸的偶发红。这里等缓存里出现 (SEQ, CMD) 再投下一帧，
 *  既消除竞态，也顺带证明"判重键确实存在"。 */
static bool wait_dup_ready(uint16_t seq, uint8_t cmd, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 1) {
        if (s_last_reply_valid && (s_last_reply_cmd == cmd) &&
            (s_last_reply_seq[0] == (uint8_t)(seq >> 8)) &&
            (s_last_reply_seq[1] == (uint8_t)seq))
            return true;
        usleep(1000);
    }
    return false;
}

/** @brief 挂死保护（lint R9）：任务用例起线程 + 等待，卡住时立刻以非 0 退出，
 *         而不是让 `make test` 永远挂在那里 */
static void on_hang(int sig)
{
    (void)sig;
    printf("\n\033[31m✘ 超时：LCSP 任务用例挂死\033[0m\n");
    _exit(2);
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

/* ================================================================
 *  组帧 / CRC
 * ================================================================ */

/** @brief CRC 哨兵与组帧字节级对拍
 *
 *  **为什么需要**：CRC 覆盖范围（SEQ 起、不含帧头、不含 CRC 自身）与**低字节在前**
 *  这两个约定各写一遍就有两套，错了不会报错 —— 设备发出的帧还是"合法"的，只是上位机
 *  全部判坏帧（现场表现：链路通、命令全超时）。哨兵 0x906E 是 CRC-16/X-25 的标准向量，
 *  与实现无关，能独立证明"算法本身没被换掉"。 */
static void case_crc_sentinel(void)
{
    TEST_BEGIN("CRC 哨兵：CRC-16/X-25 对 \"123456789\" 必须得 0x906E");

    CHECK_MSG(crc16_xmodem((const uint8_t *)"123456789", 9) == 0x906E,
              "CRC-16/X-25 标准向量应为 0x906E，得到 %04X",
              (unsigned)crc16_xmodem((const uint8_t *)"123456789", 9));
    /* 少一个字节必须变值：证明它不是"对任何输入都返回常数"的假实现 */
    CHECK_MSG(crc16_xmodem((const uint8_t *)"12345678", 8) != 0x906E,
              "长度不同的输入不应得到同一个 CRC（算法没真正读数据）");
}

/** @brief 组帧与协议附录 A 逐字节对拍（0x90、0x40 成功/失败、0x04、0x05、0x06）
 *
 *  **为什么需要**：这是唯一一条不依赖本工程任何中间量的断言 —— 期望值直接抄协议
 *  附录 A。命令字高低半字节互换、SEQ 原样、LEN 大端、CRC 低字节在前，四件事只要有一件
 *  反了，帧就"看起来合法"地被上位机丢掉。
 *
 *  0x05 的样板同时钉住**没有动画字节**（协议本轮已删掉它）：DATA = 槽位(1) + 颜色(1)
 *  + 点阵。若哪天有人按旧文档补回一个"动画"字节，LEN 会从 0x000A 变 0x000B，这条立刻红。 */
static void case_build_frame_golden(void)
{
    TEST_BEGIN("组帧：协议附录 A 的样板帧逐字节相等（0x90/0x40/0x04/0x05/0x06）");

    static const uint8_t status_ok = 0x00;
    uint8_t              out[LCSP_FRAME_MAX_LEN];

    /* 0x90 重启应答，SEQ=0x0008，DATA={00} */
    static const uint8_t golden_reboot[] = {0x55, 0xAA, 0x00, 0x08, 0x90, 0x00,
                                            0x01, 0x00, 0xB8, 0x5A};
    uint8_t              seq8[2]         = {0x00, 0x08};
    uint16_t             n = app_lcsp_build_frame(0x90, seq8, &status_ok, 1, out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_reboot)) && (0 == memcmp(out, golden_reboot, sizeof(golden_reboot))),
              "0x90 应答应为 55 AA 00 08 90 00 01 00 B8 5A（得到 %u 字节，CRC=%02X%02X）",
              (unsigned)n, out[9], out[8]);

    /* 0x40 填充应答，SEQ=0x0002 */
    static const uint8_t golden_fill[] = {0x55, 0xAA, 0x00, 0x02, 0x40, 0x00,
                                          0x01, 0x00, 0x68, 0xEE};
    uint8_t              seq2[2]       = {0x00, 0x02};
    n = app_lcsp_build_frame(0x40, seq2, &status_ok, 1, out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_fill)) && (0 == memcmp(out, golden_fill, sizeof(golden_fill))),
              "0x40 应答应为 55 AA 00 02 40 00 01 00 68 EE（得到 CRC=%02X%02X）", out[9],
              out[8]);

    /* 请求方向同样走这条组帧（测试与上位机模拟都用它）：0x04，DATA={01} */
    static const uint8_t golden_req_fill[] = {0x55, 0xAA, 0x00, 0x02, 0x04, 0x00,
                                              0x01, 0x01, 0xBA, 0x9B};
    const uint8_t        color1            = 0x01;
    n = app_lcsp_build_frame(0x04, seq2, &color1, 1, out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_req_fill)) &&
                  (0 == memcmp(out, golden_req_fill, sizeof(golden_req_fill))),
              "0x04 请求应为 55 AA 00 02 04 00 01 01 BA 9B（得到 CRC=%02X%02X）", out[9],
              out[8]);

    /* 0x05 显示图片请求：SEQ=0x0010，DATA = 槽位(0) + 颜色(1) + 8 字节点阵（**无动画字节**）*/
    static const uint8_t golden_show_image[] = {0x55, 0xAA, 0x00, 0x10, 0x05, 0x00, 0x0A, 0x00, 0x01,
                                                0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x7F,
                                                0xA1};
    static const uint8_t image_data[10] = {0x00, 0x01, 0xAA, 0xAA, 0xAA, 0xAA,
                                           0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t              seq16[2]       = {0x00, 0x10};
    n = app_lcsp_build_frame(0x05, seq16, image_data, (uint16_t)sizeof(image_data), out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_show_image)) &&
                  (0 == memcmp(out, golden_show_image, sizeof(golden_show_image))),
              "0x05 请求应为 55 AA 00 10 05 00 0A 00 01 + 8×AA + 7F A1（得到 CRC=%02X%02X）",
              out[18], out[17]);

    /* 0x06 显示已存图片：DATA 只有槽位号 1 字节（同样**无动画字节**）*/
    static const uint8_t golden_show_slot[] = {0x55, 0xAA, 0x00, 0x05, 0x06, 0x00,
                                               0x01, 0x01, 0x10, 0x92};
    uint8_t              seq5[2]            = {0x00, 0x05};
    const uint8_t        slot1              = 0x01;
    n = app_lcsp_build_frame(0x06, seq5, &slot1, 1, out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_show_slot)) &&
                  (0 == memcmp(out, golden_show_slot, sizeof(golden_show_slot))),
              "0x06 请求应为 55 AA 00 05 06 00 01 01 10 92（得到 CRC=%02X%02X）", out[9],
              out[8]);

    /* 0x40 的**失败**应答：状态码是 DATA[0]（0x01），不是"命令码 +1"之类的东西 */
    static const uint8_t golden_fill_fail[] = {0x55, 0xAA, 0x00, 0x02, 0x40, 0x00,
                                               0x01, 0x01, 0xE1, 0xFF};
    const uint8_t        status_fail        = 0x01;
    n = app_lcsp_build_frame(0x40, seq2, &status_fail, 1, out, sizeof(out));
    CHECK_MSG((n == sizeof(golden_fill_fail)) &&
                  (0 == memcmp(out, golden_fill_fail, sizeof(golden_fill_fail))),
              "0x40 失败应答应为 55 AA 00 02 40 00 01 01 E1 FF（得到 CRC=%02X%02X）", out[9],
              out[8]);

    /* 容量不足：必须返回 0 而不是写穿调用方缓冲 */
    CHECK_MSG(app_lcsp_build_frame(0x90, seq8, &status_ok, 1, out, 9) == 0,
              "out_cap 不足一帧（9 < 10）时应返回 0");    /* DATA 超协议上限（8192）→ 拒绝，不组一条非法长帧 */
    CHECK_MSG(app_lcsp_build_frame(0x90, seq8, &status_ok, (uint16_t)(LCSP_DATA_MAX_LEN + 1U), out,
                                   (uint16_t)sizeof(out)) == 0,
              "DATA 超过 LCSP_DATA_MAX_LEN 时应返回 0");
}

/* ================================================================
 *  探针
 * ================================================================ */

/** @brief 帧头错 / 长度域越界 / 字节不足 → FAKE 或 WAIT（前者跳 1 字节重找，后者等）
 *
 *  **为什么需要**：这三种处置分错一类就是"缓冲区只进不出"（该 WAIT 的判成 FAKE 会把
 *  半帧当噪声逐字节啃掉）或"等一个永远不会来的长度"（LEN>8192 判成 WAIT 后设备永久
 *  卡在这一帧上，后续所有命令都不响应）。 */
static void case_probe_header_and_short(void)
{
    TEST_BEGIN("探针：字节不足→WAIT、帧头错→FAKE、LEN>8192→FAKE");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = frame_make(s_frame, 0x01, 0x0101, nullptr, 0);
    CHECK_MSG(n == 9, "LEN=0 的最短帧应为 9 字节，得到 %u", (unsigned)n);

    /* 不足最小帧（9 字节）→ WAIT，且不消费 */
    fixture_reset();
    lcsp_feed(s_frame, 8);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_WAIT,
              "只有 8 字节（不足最短帧）时应 WAIT");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == 8, "WAIT 不得消费缓冲");

    /* 帧头错 → FAKE（框架据此前进 1 字节重新同步） */
    uint8_t bad[LCSP_FRAME_MAX_LEN];
    memcpy(bad, s_frame, n);
    bad[0] = 0xAA;
    bad[1] = 0x55;
    fixture_reset();
    lcsp_feed(bad, n);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE,
              "帧头 55 AA 顺序反了应 FAKE");

    /* LEN = 8193（> 协议上限）→ FAKE：**不能** WAIT，否则会等一个永远不来的长度 */
    fixture_reset();
    memset(s_frame, 0, 9);
    s_frame[0] = 0x55;
    s_frame[1] = 0xAA;
    s_frame[5] = 0x20; /* 0x2001 = 8193 */
    s_frame[6] = 0x01;
    lcsp_feed(s_frame, 9);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE,
              "LEN=8193 超过 8192 上限应 FAKE（判 WAIT 会永久卡住）");
}

/** @brief 分段到达：整帧到齐之前必须 WAIT（CRC 只能对完整帧算）
 *
 *  **为什么需要**：这是真实缺陷的回归哨兵。TCP 会把一整帧拆成多段送上来；若在
 *  "整帧到齐"闸门之前先比 CRC，每一段都会被判坏帧 —— 现场表现是"整屏图片（0x05）
 *  永远不生效，小命令都正常"，因为只有大帧才会跨段。 */
static void case_probe_partial_frame(void)
{
    TEST_BEGIN("探针：大帧分两段到达时先 WAIT、补齐后 READY");

    static uint8_t payload[1000];
    memset(payload, 0x5A, sizeof(payload));

    uint16_t n = frame_make(s_frame, 0x05, 0x0202, payload, (uint16_t)sizeof(payload));
    CHECK_MSG(n == 1009, "1000 字节 DATA 的帧应为 1009 字节，得到 %u", (unsigned)n);

    uint32_t tl = 0;
    uint8_t  ax = 0;

    fixture_reset();
    lcsp_feed(s_frame, 300); /* 帧头 + LEN 已到，整帧未到 */
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_WAIT,
              "整帧未到齐时应 WAIT（先比 CRC 会把每个分段都判坏帧）");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == 300, "WAIT 不得消费已到的半帧");

    lcsp_feed(s_frame + 300, (uint16_t)(n - 300));
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY,
              "补齐后应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(ax == 0x05, "aux 应为命令字 05，得到 %02X", ax);
}

/** @brief CRC 不过 → FAKE（不是 SKIP），且只推进 1 字节重找
 *
 *  **为什么需要**：把 CRC 失败写成 SKIP 会让框架按**伪长度**整段跳过，把同一批里
 *  后面的真帧一起啃掉（协议 3.3 的失步处理就是"按字节滑动重找"）。坏帧内部嵌一条
 *  真帧的场景正好把两种处置区分开：FAKE 能找到它，SKIP 会连坐吞掉。 */
static void case_probe_crc_fail(void)
{
    TEST_BEGIN("探针：CRC 不过 → FAKE，坏帧逐字节重跳后仍能找回嵌入的真帧");

    static uint8_t junk[600];
    memset(junk, 0x11, sizeof(junk));

    uint16_t n_bad = frame_make(s_frame, 0x05, 0x0303, junk, (uint16_t)sizeof(junk));
    uint8_t  good[64];
    uint16_t n_good = frame_make(good, 0x06, 0x0304, nullptr, 0);
    CHECK_MSG((n_bad == 609) && (n_good == 9), "构造帧长不对（%u / %u）", (unsigned)n_bad,
              (unsigned)n_good);

    memcpy(&s_frame[100], good, n_good); /* 坏帧内部嵌一条真帧 */
    s_frame[n_bad - 1] ^= 0x01;          /* 只破坏 CRC 尾 */

    uint32_t tl = 0;
    uint8_t  ax = 0;

    fixture_reset();
    lcsp_feed(s_frame, n_bad);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE,
              "CRC 不过必须 FAKE —— 返回 SKIP 会按伪长度吞掉后面的字节");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == n_bad,
              "FAKE 不消费（框架只前进 1 字节），得到剩余 %u",
              (unsigned)rb_avail(&s_ut_rb, nullptr));

    /* 模拟框架对 FAKE 的处置：跳 1 字节重探，直到认出嵌入的真帧 */
    bool found = false;
    for (int guard = 0; guard < 300; guard++) {
        app_pcb_probe_state_t st = lcsp_probe(&tl, &ax, sizeof(s_scratch));
        if (st == APP_PCB_PROBE_STATE_READY) {
            found = true;
            break;
        }
        if (st != APP_PCB_PROBE_STATE_FAKE)
            break; /* WAIT / SKIP 都不该出现 */
        rb_skip(&s_ut_rb, 1, nullptr);
    }
    CHECK_MSG(found, "坏帧之后的重跳没能找回嵌入的真帧");
    CHECK_MSG((tl == n_good) && (ax == 0x06), "找回的应是嵌入真帧（LEN=%u / cmd=%02X）",
              (unsigned)tl, ax);
}

/** @brief READY 契约：total_len / aux / **只窥视**；含最短帧与最长帧
 *
 *  **为什么需要**：探针的返回值直接决定框架消费多少字节。total_len 少一字节会漏掉
 *  CRC 尾（永远 CRC 错），多一字节会吃掉下一帧的帧头；`aux` 是任务拿到的命令字段。
 *  最长帧（0x05 满整屏位图 2802 字节 = 帧 2811）必须放得进 `FRAME_DATA_MAX_LEN`，
 *  否则整屏图这条产品主功能在框架层就被截断。 */
static void case_probe_ready_contract(void)
{
    TEST_BEGIN("探针 READY：total_len=9+LEN、aux=命令字、只窥视；最短/最长帧都能认");

    uint32_t tl = 0;
    uint8_t  ax = 0;

    /* 最短：LEN=0 → 9 字节 */
    uint16_t n = frame_make(s_frame, 0x09, 0x0401, nullptr, 0);
    fixture_reset();
    lcsp_feed(s_frame, n);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY,
              "LEN=0 的 9 字节帧应 READY");
    CHECK_MSG((tl == 9U) && (ax == 0x09), "最短帧应为 total=9 / aux=09（得到 %u / %02X）",
              (unsigned)tl, ax);
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == 9, "READY 也只窥视，消费由框架做");

    /* 最长：DATA = LCSP_PAYLOAD_MAX_LEN（2 + 2800）→ 帧 2811 */
    static uint8_t big[LCSP_PAYLOAD_MAX_LEN];
    memset(big, 0xA5, sizeof(big));
    n = frame_make(s_frame, 0x05, 0x0402, big, (uint16_t)sizeof(big));
    CHECK_MSG(n == LCSP_FRAME_MAX_LEN, "满整屏图帧应为 %u 字节，得到 %u",
              (unsigned)LCSP_FRAME_MAX_LEN, (unsigned)n);

    fixture_reset();
    lcsp_feed(s_frame, n);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY,
              "满整屏图帧应 READY（0x05 是本协议唯一的一帧一幅图）");
    CHECK_MSG((tl == n) && (ax == 0x05), "满帧 total 应为 %u / aux=05（得到 %u / %02X）",
              (unsigned)n, (unsigned)tl, ax);
}

/** @brief 暂存区契约：装不下 → SKIP 并报出整帧长度（**不是** FAKE）
 *
 *  **为什么需要**：SKIP 的语义是"结构合法、本机不处理，按整帧长度前进"；FAKE 会让框架
 *  只前进 1 字节，把整帧的载荷当噪声逐字节重扫（最大 2.8KB × 2.8KB 的 O(n²)）。
 *  两者都报 total_len，但只有 SKIP 会被整段跳过 —— 写反了就是"设备突然卡死几秒"。 */
static void case_probe_skip_scratch(void)
{
    TEST_BEGIN("探针：帧长超暂存区 → SKIP 且报出整帧长度；恰好装下 → READY");

    static uint8_t payload[2000];
    memset(payload, 0x33, sizeof(payload));

    uint16_t n = frame_make(s_frame, 0x05, 0x0501, payload, (uint16_t)sizeof(payload));
    CHECK_MSG(n == 2009, "2000 字节 DATA 的帧应为 2009 字节，得到 %u", (unsigned)n);

    uint32_t tl = 0;
    uint8_t  ax = 0;

    fixture_reset();
    memset(s_scratch, 0xEE, 64);
    lcsp_feed(s_frame, n);
    CHECK_MSG(lcsp_probe(&tl, &ax, 16) == APP_PCB_PROBE_STATE_SKIP,
              "暂存区 16 字节装不下 2009 字节帧应 SKIP（不是 FAKE）");
    CHECK_MSG(tl == n, "SKIP 也要报出整帧长度（%u），得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(s_scratch[16] == 0xEE, "SKIP 不得把整帧拷进暂存区（框架会整段跳过）");

    tl = 0;
    CHECK_MSG(lcsp_probe(&tl, &ax, n) == APP_PCB_PROBE_STATE_READY, "暂存区恰好装下应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief 互不毒化 + 重同步：其它协议字节流不得判成 LCSP 帧；帧前一个杂字节也要能找回
 *
 *  **为什么需要**：LCSP 与 IAP/LDI 共用同一条以太网/UDP 通道族。若探针把别家的流当
 *  自己的帧（或反之），后果是双向的：自己的帧被别家吞掉，别家的帧被自己按伪长度跳过。
 *  重同步这条钉的是"帧头前有杂字节时，框架跳 1 字节就能重新对上" —— 这是所有现场
 *  "偶发丢帧"最后都依赖的机制。 */
static void case_probe_no_cross_poison(void)
{
    TEST_BEGIN("探针：IAP/LDI 字节流不得判 LCSP；帧前杂字节跳 1 字节后可重同步");

    uint32_t tl = 0;
    uint8_t  ax = 0;

    static const struct {
        const char *name;
        uint8_t     head[4];
    } others[] = {
        {"IAP (5A5A5A5A)", {0x5A, 0x5A, 0x5A, 0x5A}},
        {"LDI (FFFF)", {0xFF, 0xFF, 0x00, 0x00}},
        {"RLS (FFFE)", {0xFF, 0xFE, 0x00, 0x00}},
    };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        fixture_reset();
        uint8_t buf[64];
        memset(buf, 0x11, sizeof(buf));
        memcpy(buf, others[i].head, 4);
        lcsp_feed(buf, sizeof(buf));
        CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE,
                  "%s 的字节流被判成 LCSP 帧", others[i].name);
    }

    /* 一个杂字节 + 一条好帧：第一次 FAKE，跳 1 字节后认出来 */
    uint16_t n = frame_make(s_frame, 0x03, 0x0601, nullptr, 0);
    fixture_reset();
    const uint8_t junk = 0x00;
    lcsp_feed(&junk, 1);
    lcsp_feed(s_frame, n);

    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE,
              "帧头前有杂字节时应 FAKE（框架前进 1 字节）");
    rb_skip(&s_ut_rb, 1, nullptr);
    CHECK_MSG(lcsp_probe(&tl, &ax, sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY,
              "跳 1 字节后应重新对上真帧（重同步）");
    CHECK_MSG((tl == n) && (ax == 0x03), "重同步后的帧应为 total=%u / aux=03（得到 %u / %02X）",
              (unsigned)n, (unsigned)tl, ax);
}

/* ================================================================
 *  命令层：0x01 设备信息
 * ================================================================ */

/** @brief 0x01 设备信息：序列号 / 几何 / 槽位数 / 亮度 / 状态位（15 字节 RDATA）
 *
 *  **为什么需要**：这是上位机发现设备的唯一入口（协议 7.1），字段错位不会有任何报错：
 *  序列号错 → 现场"按序列号建台账"时两台设备重名（发现列表只剩一台）；几何错 →
 *  上位机按错的分辨率发图，0x05 一律被长度校验拒掉，看起来像"设备不支持显示图片"；
 *  槽位数报成协议上限（16）→ 上位机会一直下发本机存不下的槽位，0x05/0x06 全部回 0x01。
 *  亮度按**硬件实际值**上报（关屏时 0），否则上位机会看到"亮度 80 但屏是灭的"。 */
static void case_cmd_device_info(void)
{
    TEST_BEGIN("0x01 设备信息：序列号/几何/槽位数/亮度/状态位；无记录时用默认序列号");

    stubs_reset();

    /* 有记录：序列号取自记录（8 字节 ASCII，不足补 0），几何取自 app_screen_* */
    s_cfg_valid = true;
    memcpy(s_cfg.serial, "LCSP0001", LCSP_SERIAL_LEN);
    s_cfg.brightness = 100U;
    s_cfg.oe_enabled = 1U;

    uint16_t n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0701, nullptr, 0);
    CHECK_MSG((n == 25U) && (s_reply[4] == APP_LCSP_REPLY_DEVICE_INFO) && (s_reply[5] == 0U) &&
                  (s_reply[6] == 16U) && (s_reply[7] == APP_LCSP_STATUS_OK),
              "应答应为 0x10 / LEN=16（状态 + 15 字节 RDATA）/ 状态 0（得到 %u 字节 LEN=%02X%02X status=%02X）",
              (unsigned)n, s_reply[5], s_reply[6], s_reply[7]);
    CHECK_MSG(0 == memcmp(&s_reply[8], "LCSP0001", LCSP_SERIAL_LEN),
              "RDATA[0..7] 应为记录里的序列号（得到 %.8s）", (const char *)&s_reply[8]);
    CHECK_MSG((s_reply[16] == 0x00 && s_reply[17] == 0x08 && s_reply[18] == 0x00 &&
               s_reply[19] == 0x02),
              "几何应为 rows=8 / cols=2（大端），得到 %02X%02X / %02X%02X", s_reply[16],
              s_reply[17], s_reply[18], s_reply[19]);
    /* 槽位数报**本机真实可用数量**（LCSP_SLOT_MAX = 3），不是协议上限 16：
       上位机按它校验 0x05/0x06 的槽位号，报大了就会一直发本机存不下的槽位。 */
    CHECK_MSG(s_reply[20] == (uint8_t)LCSP_SLOT_MAX,
              "RDATA[12] 应为本机槽位数 %u（不得写死协议上限 16），得到 %u",
              (unsigned)LCSP_SLOT_MAX, s_reply[20]);
    CHECK_MSG(s_reply[21] == 100U, "硬件亮度档 7 应上报 100%%，得到 %u", s_reply[21]);
    CHECK_MSG(s_reply[22] == 0x01U, "状态应为 0x01（屏开、无已存图片），得到 %02X", s_reply[22]);

    /* 无记录：必须回落到默认记录（序列号 LCSP_SERIAL_DEFAULT），不能是一串 0 */
    s_cfg_valid = false;
    n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0702, nullptr, 0);
    CHECK_MSG((n == 25U) && (0 == memcmp(&s_reply[8], LCSP_SERIAL_DEFAULT, LCSP_SERIAL_LEN)),
              "无有效记录时应回落到默认序列号（得到 %.8s）", (const char *)&s_reply[8]);

    /* 现状哨兵：0x01 是只读命令，**刻意不校验 DATA 长度**（发现类工具可能带填充；
       多带几个字节就拒答会让上位机连"设备在不在"都问不出来）。将来若改成严格校验，
       这条期望要一并改 —— 那不是本模块单方面能决定的。 */
    static const uint8_t padding[3] = {0xDE, 0xAD, 0xBE};
    n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0703, padding, (uint16_t)sizeof(padding));
    CHECK_MSG((n == 25U) && (s_reply[4] == APP_LCSP_REPLY_DEVICE_INFO) &&
                  (s_reply[7] == APP_LCSP_STATUS_OK),
              "带填充的 0x01 也必须正常应答（现状口径：只读命令不校验长度）");
}

/** @brief 0x01 状态位：bit0 = 屏输出、bit1 = 任一槽位有图；亮度按硬件档换算
 *
 *  **为什么需要**：状态位是上位机决定"能不能发图/是不是黑屏"的唯一依据；两位写反或
 *  恒 0 会让它去排查错误的方向。亮度换算（档 1–7 → 0–100）错了则上位机判"屏太暗"、
 *  反复下发 0x07，把现场已有的固定亮度覆盖掉。 */
static void case_cmd_device_info_state(void)
{
    TEST_BEGIN("0x01 状态位：OE 位 / 槽位位 / 硬件亮度档 → 百分比");

    stubs_reset();
    s_cfg_valid = true;

    /* 关屏 + 有图：0b10；硬件亮度档 4 → 4*100/7 = 57（整数除，与实现同口径） */
    s_oe_state  = false;
    s_bright_hw = 4U;
    static const uint8_t bm[2] = {0xAA, 0x55};
    slot_seed(2, 8U, 2U, 1U, bm, 2U);

    uint16_t n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0801, nullptr, 0);
    CHECK_MSG(n == 25U, "0x01 应答应为 25 字节（状态 + 15 字节 RDATA），得到 %u", (unsigned)n);
    CHECK_MSG(s_reply[22] == 0x02U, "关屏 + 有图时状态应为 0x02，得到 %02X", s_reply[22]);
    CHECK_MSG(s_reply[21] == 57U, "硬件档 4 应上报 57%%，得到 %u", s_reply[21]);

    /* 开屏 + 无图：0b01；最亮档 7 → 100% */
    s_oe_state  = true;
    s_bright_hw = 7U;
    slot_stub_reset();
    n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0802, nullptr, 0);
    CHECK_MSG(s_reply[22] == 0x01U, "开屏 + 无图时状态应为 0x01，得到 %02X", s_reply[22]);
    CHECK_MSG(s_reply[21] == 100U, "硬件档 7 应上报 100%%，得到 %u", s_reply[21]);
}

/** @brief 0x01 应答与协议附录 A.17 的样板帧逐字节对拍（仅槽位数/亮度按本机取值）
 *
 *  **为什么需要**：15 字节 RDATA 的**字段顺序**是本轮协议改动的核心（槽位数插在几何与
 *  亮度之间），错一位就会让上位机把"屏高"读成"槽位数"。附录 A.17 的样板给的是
 *  "序列号 chuangdi、240×48、16 槽位、亮度 80、状态 1"；本机槽位固定 `LCSP_SLOT_MAX`（3），
 *  亮度由硬件档位量化（档 6 → 85，到不了样板的 80），所以**只替换这两个字节并重算 CRC**，
 *  其余 22 个字节必须与协议样板逐字节相同 —— 这既钉住布局，又不把协议上限 16 写死。 */
static void case_cmd_device_info_golden_a17(void)
{
    TEST_BEGIN("0x01 设备信息：与附录 A.17 样板逐字节对拍（槽位数取 LCSP_SLOT_MAX）");

    stubs_reset();

    /* 让本机能复现样板里的其余字段：序列号取出厂值 `chuangdi`（协议 7.1 已改为全系列统一值，
       与 A.17 样板一致）、逻辑屏 240×48、屏开、无存图 */
    s_cfg_valid = true;
    memcpy(s_cfg.serial, LCSP_SERIAL_DEFAULT, LCSP_SERIAL_LEN);
    s_screen_rows = 240U;
    s_screen_cols = 48U;
    s_bright_hw   = 6U; /* 6*100/7 = 85 */
    s_oe_state    = true;

    uint16_t n = cmd_run(APP_LCSP_CMD_DEVICE_INFO, 0x0004, nullptr, 0);

    static const uint8_t a17[] = {0x55, 0xAA, 0x00, 0x04, 0x10, 0x00, 0x10,
                                  0x00,                                     /* 状态码 */
                                  0x63, 0x68, 0x75, 0x61, 0x6E, 0x67, 0x64, 0x69, /* "chuangdi" */
                                  0x00, 0xF0,                               /* 屏宽 240 */
                                  0x00, 0x30,                               /* 屏高 48 */
                                  0x10,                                     /* 样板：16 槽位 */
                                  0x50,                                     /* 样板：亮度 80 */
                                  0x01,                                     /* 状态：屏开、无图 */
                                  0xE5, 0xD1};                              /* 样板 CRC */
    uint8_t want[sizeof(a17)];
    memcpy(want, a17, sizeof(a17));

    /* 把"本机决定"的两个字节换成实际值，并按协议算法重算 CRC */
    want[20]     = (uint8_t)LCSP_SLOT_MAX;
    want[21]     = 85U; /* 档 6 → 6*100/7 = 85（样板 80 是协议给的示例值，档位量化到不了）*/
    uint16_t crc = crc16_xmodem(&want[2], 5U + 16U);
    want[23]     = (uint8_t)(crc & 0xFFU);
    want[24]     = (uint8_t)(crc >> 8);

    CHECK_MSG((n == sizeof(want)) && (0 == memcmp(s_reply, want, sizeof(want))),
              "0x01 应答应与 A.17 样板只差槽位数/亮度两字节（得到 %u 字节，RDATA[12]=%u）",
              (unsigned)n, s_reply[20]);
    CHECK_MSG(s_reply[20] == (uint8_t)LCSP_SLOT_MAX,
              "槽位数必须报本机的 %u，不得写死协议上限 16（得到 %u）", (unsigned)LCSP_SLOT_MAX,
              s_reply[20]);
}

/* ================================================================
 *  命令层：0x02 设置 IP
 * ================================================================ */

/** @brief 0x02 设置 IP：落盘入参 / 应答取**回读** / IAP 镜像
 *
 *  **为什么需要**：0x02 是唯一能把设备改到"连不上"的命令（改完要重启才生效）。协议 7.2
 *  要求应答带回**从 Flash 读回**的配置：回显只能证明"收到了"，读回才证明"落盘了" ——
 *  这是重启前唯一可验证的环节。所以桩故意让读回值与请求**不同**，应答必须跟读回走；
 *  两边一样的话这条断言恒真，测不出实现走的是哪一条。 */
static void case_cmd_set_ip(void)
{
    TEST_BEGIN("0x02 设置 IP：保存入参 / 应答取回读值 / IAP 镜像端口");

    stubs_reset();

    static const uint8_t req_ip[4]   = {192, 168, 1, 50};
    static const uint8_t req_mask[4] = {255, 255, 255, 0};
    static const uint8_t req_gw[4]   = {192, 168, 1, 1};
    uint8_t              payload[14];

    ip_payload(payload, req_ip, req_mask, req_gw, 0x2724U); /* 10020 */

    /* 读回值故意与请求不同（端口 0x271B = 10011）：应答若回显请求，端口那条会红 */
    s_cfg_valid             = true;
    s_cfg_readback_override = true;
    s_cfg_readback_port[0]  = 0x27;
    s_cfg_readback_port[1]  = 0x1B;

    uint16_t n = cmd_run(APP_LCSP_CMD_SET_IP, 0x0901, payload, 14);

    CHECK_MSG((n == 24U) && (s_reply[4] == APP_LCSP_REPLY_SET_IP) && (s_reply[5] == 0U) &&
                  (s_reply[6] == 15U) && (s_reply[7] == APP_LCSP_STATUS_OK) &&
                  (s_reply[2] == 0x09) && (s_reply[3] == 0x01),
              "应答应为 0x20/LEN=15/状态 0 且 SEQ 原样回显（得到 cmd=%02X status=%02X seq=%02X%02X）",
              s_reply[4], s_reply[7], s_reply[2], s_reply[3]);
    CHECK_MSG((s_cfg_save_calls == 1) && (0 == memcmp(s_cfg_saved.ip, req_ip, 4)) &&
                  (0 == memcmp(s_cfg_saved.mask, req_mask, 4)) &&
                  (0 == memcmp(s_cfg_saved.gw, req_gw, 4)) &&
                  (0 == memcmp(s_cfg_saved.port, &payload[12], 2)),
              "落盘入参应为请求的 ip/mask/gw/port（保存 %d 次）", s_cfg_save_calls);
    CHECK_MSG((0 == memcmp(&s_reply[8], req_ip, 4)) && (0 == memcmp(&s_reply[12], req_mask, 4)) &&
                  (0 == memcmp(&s_reply[16], req_gw, 4)),
              "应答 DATA 前 12 字节应为读回的 ip/mask/gw");
    CHECK_MSG((s_reply[20] == 0x27) && (s_reply[21] == 0x1B),
              "应答端口必须取**读回**值 0x271B 而不是回显 0x2724（得到 %02X%02X）", s_reply[20],
              s_reply[21]);
    CHECK_MSG((s_iap_calls == 1) && (0 == memcmp(s_iap_ip, req_ip, 4)) && (s_iap_port == 10011U),
              "IAP 记录镜像应写入读回的 ip 与解码后的端口 10011（调用 %d 次 / 端口 %u）",
              s_iap_calls, (unsigned)s_iap_port);

    /* 落盘失败：必须回 0x01，且**不更新** IAP 镜像 —— 镜像写的是"下次启动要用的值"，
       配置没写进去却镜像了， Recovery 上报的就是一份并不存在的配置。 */
    stubs_reset();
    s_cfg_valid    = true;
    s_cfg_save_ret = -1;
    (void)cmd_run(APP_LCSP_CMD_SET_IP, 0x0902, payload, 14);
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_IP, APP_LCSP_STATUS_FAIL),
              "落盘失败应回 0x20/状态 01（得到 status=%02X）", s_reply[7]);
    CHECK_MSG((s_iap_calls == 0) && (s_cfg_save_calls == 1),
              "落盘失败不得更新 IAP 镜像（save=%d iap=%d）", s_cfg_save_calls, s_iap_calls);
}

/** @brief 0x02 越界矩阵：非法 IP / 非连续掩码 / 端口 0 / 长度≠14 → 0x01 且**零副作用**
 *
 *  **为什么需要**：这些值一旦被接受，设备重启后就不在网了（0.0.0.0 不可路由、
 *  224.x 是组播、127.x 是环回、255.255.255.x 是广播、非连续掩码等于把整个网段
 *  划进同一条链路）—— 现场没有任何远程手段能救回来，只能跑去现场按复位/重刷。
 *  所以"拒绝"必须同时断言"一个字节都没落盘"：先落盘后校验的实现会留下坏记录。 */
static void case_cmd_set_ip_bounds(void)
{
    TEST_BEGIN("0x02 越界矩阵：非法 IP / 掩码 / 端口 / 长度 → 0x20 状态 01 且零副作用");

    static const struct {
        uint8_t     ip[4];
        uint8_t     mask[4];
        uint8_t     gw[4];
        uint16_t    port;
        uint16_t    plen;
        const char *why;
    } bad[] = {
        {{0, 0, 0, 0}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 14, "IP 全零"},
        {{224, 0, 0, 1}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 14, "IP 首字节 224（组播/保留）"},
        {{255, 255, 255, 1}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 14, "IP 255.255.255.x（广播）"},
        {{127, 0, 0, 1}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 14, "IP 127.x（环回）"},
        {{192, 168, 1, 50}, {255, 0, 255, 0}, {192, 168, 1, 1}, 10028, 14, "掩码非连续"},
        {{192, 168, 1, 50}, {0, 0, 0, 0}, {192, 168, 1, 1}, 10028, 14, "掩码全零"},
        {{192, 168, 1, 50}, {255, 255, 255, 0}, {192, 168, 1, 1}, 0, 14, "端口 0"},
        {{192, 168, 1, 50}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 13, "DATA 少 1 字节"},
        {{192, 168, 1, 50}, {255, 255, 255, 0}, {192, 168, 1, 1}, 10028, 15, "DATA 多 1 字节"},
    };

    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        s_cfg_valid = true; /* 有记录也不许被改 */
        ip_payload(payload, bad[i].ip, bad[i].mask, bad[i].gw, bad[i].port);
        (void)cmd_run(APP_LCSP_CMD_SET_IP, 0x0900U + (uint16_t)i, payload, bad[i].plen);
        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_IP, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x20/状态 01（得到 cmd=%02X status=%02X len=%u）", bad[i].why,
                  s_reply[4], s_reply[7], (unsigned)s_req.reply_len);
        CHECK_MSG((s_cfg_save_calls == 0) && (s_iap_calls == 0),
                  "越界（%s）不得落盘/镜像（save=%d iap=%d）", bad[i].why, s_cfg_save_calls,
                  s_iap_calls);
    }
}

/* ================================================================
 *  命令层：0x03 查询 IP
 * ================================================================ */

/** @brief 0x03 查询 IP：应答走广播，且报**运行态**而不是记录
 *
 *  **为什么需要**：这条是"设备还在不在网上"的探测（协议 7.3）。两个点一起错就没救：
 *  ① 回单播在跨网段/NAT 时到不了上位机；② 报记录而不是运行态，会在 0x02 刚落盘、
 *  还没重启的窗口里给出一个**当前并不通**的地址，上位机按它去连必然超时。
 *  桩把记录与运行态设成不同值，两条断言都咬得住。 */
static void case_cmd_query_ip(void)
{
    TEST_BEGIN("0x03 查询 IP：应答广播 + 报运行态（不是记录）");

    stubs_reset();

    /* 记录里是"下次启动才生效"的值 */
    s_cfg_valid = true;
    static const uint8_t rec_ip[4] = {10, 0, 0, 5};
    memcpy(s_cfg.ip, rec_ip, 4);

    /* 运行态是另一组值（pl_net_get_ip / app_tcp_server_get_port 给的） */
    static const uint8_t run_ip[4]   = {192, 168, 0, 77};
    static const uint8_t run_mask[4] = {255, 255, 0, 0};
    static const uint8_t run_gw[4]   = {192, 168, 0, 1};
    memcpy(s_net_ip, run_ip, 4);
    memcpy(s_net_mask, run_mask, 4);
    memcpy(s_net_gw, run_gw, 4);
    s_tcp_port = 10028U;

    uint16_t n = cmd_run(APP_LCSP_CMD_QUERY_IP, 0x0A01, nullptr, 0);

    CHECK_MSG((n == 24U) && (s_reply[4] == APP_LCSP_REPLY_QUERY_IP) &&
                  (s_reply[7] == APP_LCSP_STATUS_OK),
              "应答应为 0x30 / 状态 0（得到 %u 字节 cmd=%02X status=%02X）", (unsigned)n,
              s_reply[4], s_reply[7]);
    CHECK_MSG(s_req.reply_broadcast, "0x03 的应答必须置广播（单播在跨网段时回不去）");
    CHECK_MSG((0 == memcmp(&s_reply[8], run_ip, 4)) && (0 == memcmp(&s_reply[12], run_mask, 4)) &&
                  (0 == memcmp(&s_reply[16], run_gw, 4)),
              "应答应报运行态 IP/掩码/网关，而不是 Flash 记录（记录是 10.0.0.5）");
    CHECK_MSG((s_reply[20] == 0x27) && (s_reply[21] == 0x2C),
              "应答端口应为运行态的 10028（0x272C），得到 %02X%02X", s_reply[20], s_reply[21]);
}

/* ================================================================
 *  命令层：0x04 全屏填充
 * ================================================================ */

/** @brief 0x04 全屏填充：覆盖整块逻辑屏、**不持久化**；颜色码 0..7 逐档
 *
 *  **为什么需要**：填充是"清屏/点灯验屏"的瞬时动作（协议 7.4 明确不写断电保存区）。
 *  若顺手 persist 了，掉电重启后就是一屏纯色，现场再也看不到"屏体本身是否正常"这个
 *  判据。几何必须用**逻辑屏**（app_screen_rows/cols）而不是本卡实屏，否则级联时只填
 *  自己那一角（现象很像"发错了卡"）。 */
static void case_cmd_fill(void)
{
    TEST_BEGIN("0x04 全屏填充：整块逻辑屏 + 不持久化 + 颜色逐档");

    stubs_reset();

    const uint8_t red = 1U;
    uint16_t      n   = cmd_run(APP_LCSP_CMD_FILL_SCREEN, 0x0B01, &red, 1);

    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_FILL_SCREEN, APP_LCSP_STATUS_OK),
              "0x04 红色应回 0x40/状态 0（得到 cmd=%02X status=%02X）", s_reply[4], s_reply[7]);
    CHECK_MSG((s_render_calls == 1) && (s_rc_type == APP_RENDER_TYPE_FILL) && (s_rc_x == 0) &&
                  (s_rc_y == 0) && (s_rc_w == 8U) && (s_rc_h == 2U) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_RED) && (!s_rc_persist),
              "应为 FILL(0,0,8,2,RED) 且 persist=false（calls=%d w=%u h=%u color=%u persist=%d）",
              s_render_calls, s_rc_w, s_rc_h, (unsigned)s_rc_color, (int)s_rc_persist);
    CHECK_MSG((s_cfg_save_calls == 0) && (s_slot_save_calls == 0),
              "填充不得落盘（协议 7.4：只改当前显示，不写保存区）");

    /* 颜色码 7（白）是上界，必须放行；0（黑）是清屏语义 */
    const uint8_t white = 7U;
    n = cmd_run(APP_LCSP_CMD_FILL_SCREEN, 0x0B02, &white, 1);
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_FILL_SCREEN, APP_LCSP_STATUS_OK) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_WHITE),
              "颜色 7 应放行并映射为白（得到 status=%02X color=%u）", s_reply[7],
              (unsigned)s_rc_color);

    const uint8_t black = 0U;
    n = cmd_run(APP_LCSP_CMD_FILL_SCREEN, 0x0B03, &black, 1);
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_FILL_SCREEN, APP_LCSP_STATUS_OK) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_BLACK),
              "颜色 0 应放行并映射为黑（清屏），得到 status=%02X color=%u", s_reply[7],
              (unsigned)s_rc_color);
    CHECK_MSG((n == 10U) && (s_reply[2] == 0x0B) && (s_reply[3] == 0x03),
              "SEQ 必须原样回显（得到 %02X%02X）", s_reply[2], s_reply[3]);
}

/** @brief 0x04 越界矩阵：颜色码 ≥8 / 长度≠1 → 0x40 状态 01 且不画
 *
 *  **为什么需要**：颜色码 8 起是保留位（协议 7.4）；放过它会经枚举转换落到别的颜色
 *  甚至越界值。长度≠1 时若照画，读的是未初始化的 DATA —— 屏上出现随机颜色。 */
static void case_cmd_fill_bounds(void)
{
    TEST_BEGIN("0x04 越界矩阵：颜色 8 / 长度 0 与 2 → 0x40 状态 01 且不画");

    static const struct {
        uint16_t    len;
        uint8_t     color;
        const char *why;
    } bad[] = {
        {1, 8, "颜色码 8（保留）"},
        {1, 0xFF, "颜色码 0xFF"},
        {0, 0, "没有 DATA"},
        {2, 1, "多一个字节"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        const uint8_t buf[2] = {bad[i].color, 0x00};
        (void)cmd_run(APP_LCSP_CMD_FILL_SCREEN, 0x0C00 + (uint16_t)i, buf, bad[i].len);
        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_FILL_SCREEN, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x40/状态 01（得到 cmd=%02X status=%02X）", bad[i].why,
                  s_reply[4], s_reply[7]);
        CHECK_MSG((s_render_calls == 0) && (s_cfg_save_calls == 0),
                  "越界（%s）不得调 render / 落盘（render=%d save=%d）", bad[i].why,
                  s_render_calls, s_cfg_save_calls);
    }
}

/* ================================================================
 *  命令层：0x05 显示图片 / 0x06 显示已存图片
 * ================================================================ */

/** @brief 0x05 显示图片：位序翻转（协议 bit0 最左 → 工程 MSB 最左）、槽位 0 不落盘
 *
 *  **为什么需要**：协议 7.5 规定点阵 bit0 是最左像素，而本工程画布 / 驱动 / 持久化
 *  三处统一是 MSB 最左。不转的话屏上每一行左右镜像 —— 文字看着像反写、图形看着像
 *  错位，而长度与 CRC **全部正确**，抓包看不出任何异常。槽位 0 是"只显示不保存"
 *  （协议 7.5），误存会把"临时看一张图"变成擦一次扇区。 */
static void case_cmd_show_image(void)
{
    TEST_BEGIN("0x05 显示图片：LSB-first → MSB-first、槽位 0 只显示不保存");

    stubs_reset();

    /* 8×2 → 2 字节点阵；协议方向 0x01/0x00 表示"每行最左一个像素亮" */
    const uint8_t payload0[2 + 2] = {LCSP_SLOT_NONE, 1U, 0x01, 0x00};

    uint16_t n = cmd_run(APP_LCSP_CMD_SHOW_IMAGE, 0x0D01, payload0, (uint16_t)sizeof(payload0));

    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_IMAGE, APP_LCSP_STATUS_OK),
              "0x05 应回 0x50/状态 0（得到 cmd=%02X status=%02X）", s_reply[4], s_reply[7]);
    /* 两次调用：先 FILL 黑把这一整块清掉（位图渲染只写 bit=1，bit=0 不动），
       再贴 BITMAP。只贴不清的话上一幅图的亮点会原样留着 —— 现场就是"发新图没反应"。 */
    CHECK_MSG((s_render_calls == 2) && (s_rc0_type == APP_RENDER_TYPE_FILL) &&
                  (s_rc0_x == 0) && (s_rc0_y == 0) && (s_rc0_w == 8U) && (s_rc0_h == 2U) &&
                  (s_rc0_color == DEV_DISPLAY_COLOR_BLACK) && (s_rc_type == APP_RENDER_TYPE_BITMAP) &&
                  (s_rc_x == 0) && (s_rc_y == 0) && (s_rc_w == 8U) && (s_rc_h == 2U) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_RED) && (!s_rc_persist),
              "应为 FILL(0,0,8,2,黑) + BITMAP(0,0,8,2,RED) 且 persist=false"
              "（calls=%d 首调 type=%d w=%u h=%u）",
              s_render_calls, (int)s_rc0_type, s_rc0_w, s_rc0_h);
    CHECK_MSG((s_rc_bitmap_len == 2U) && (s_rc_bitmap_buf[0] == 0x80) &&
                  (s_rc_bitmap_buf[1] == 0x00),
              "位序必须翻转：0x01 → 0x80（得到 %02X %02X）", s_rc_bitmap_buf[0],
              s_rc_bitmap_buf[1]);
    CHECK_MSG(s_slot_save_calls == 0, "槽位 0 = 只显示，不得写槽位记录（save=%d 次）",
              s_slot_save_calls);

    /* 槽位 1：要落盘，persist=true，存下去的也是 MSB-first */
    const uint8_t payload1[2 + 2] = {1U, 2U, 0x80, 0x01};
    n = cmd_run(APP_LCSP_CMD_SHOW_IMAGE, 0x0D02, payload1, (uint16_t)sizeof(payload1));

    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_IMAGE, APP_LCSP_STATUS_OK) && s_rc_persist &&
                  (s_rc_color == DEV_DISPLAY_COLOR_GREEN),
              "槽位 1 应成功且 persist=true / 绿色（status=%02X persist=%d color=%u）", s_reply[7],
              (int)s_rc_persist, (unsigned)s_rc_color);
    CHECK_MSG((s_slot_save_calls == 1) && (s_slot_save_slot == 1U) && (s_slot_save_len == 2U),
              "槽位 1 应保存一次、点阵 2 字节（calls=%d slot=%u len=%u）", s_slot_save_calls,
              s_slot_save_slot, (unsigned)s_slot_save_len);
    CHECK_MSG((s_slots[1].bm[0] == 0x01) && (s_slots[1].bm[1] == 0x80) &&
                  (s_slots[1].rows == 8U) && (s_slots[1].cols == 2U),
              "槽位里存的应是翻转后的点阵 + 当前几何（%02X %02X）", s_slots[1].bm[0],
              s_slots[1].bm[1]);
    CHECK_MSG((n == 10U) && (s_reply[2] == 0x0D) && (s_reply[3] == 0x02), "SEQ 必须原样回显");

    /* 落盘失败：**显示已经生效、不回滚**，只把状态回 0x01（协议 7.5："半张旧图比
       没存住更糟"）。若实现先落盘再显示，失败时屏上是上一张图，而现场只会看到
       "图没换成功"，没人会想到是槽位写坏了。 */
    stubs_reset();
    s_slot_save_ret = -1;
    n = cmd_run(APP_LCSP_CMD_SHOW_IMAGE, 0x0D03, payload1, (uint16_t)sizeof(payload1));
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_IMAGE, APP_LCSP_STATUS_FAIL),
              "槽位保存失败应回 0x50/状态 01（得到 status=%02X）", s_reply[7]);
    CHECK_MSG((s_render_calls == 2) && (s_slot_save_calls == 1),
              "落盘失败也必须先显示（render=%d save=%d）", s_render_calls, s_slot_save_calls);
}

/** @brief 0x05 越界矩阵：槽位 / 颜色 / 长度 off-by-one / 几何超上限 → 0x50 状态 01 且零副作用
 *
 *  **为什么需要**：长度必须**恰好**等于 `2 + ceil(rows/8)*cols`（协议 7.5）。取 ≥ 会让
 *  "多出来的字节属于哪一帧"变得不可判；少一字节则下游按几何读会**越界读帧缓冲**
 *  （ASan 能抓，现场是随机花屏）。几何超过本模块位图上限时唯一正确的处置是明确拒绝，
 *  而不是按旧 stride 铺上去（那在屏上是一幅斜纹，不报错）。 */
static void case_cmd_show_image_bounds(void)
{
    TEST_BEGIN("0x05 越界矩阵：槽位 4 / 颜色 8 / 长度 off-by-one / 几何超上限 → 状态 01");

    static const struct {
        uint8_t     slot;
        uint8_t     color;
        uint16_t    plen;
        const char *why;
    } bad[] = {
        {4, 1, 4, "槽位 4（超出 3 个）"},
        {1, 8, 4, "颜色码 8（保留）"},
        {1, 1, 3, "点阵少 1 字节（2+2-1）"},
        {1, 1, 5, "点阵多 1 字节（2+2+1）"},
        {1, 1, 2, "只有槽位+颜色、没有点阵"},
    };

    uint8_t payload[6] = {1U, 1U, 0xA5, 0x5A, 0x00, 0x00};

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        payload[0] = bad[i].slot;
        payload[1] = bad[i].color;
        (void)cmd_run(APP_LCSP_CMD_SHOW_IMAGE, 0x0E00U + (uint16_t)i, payload, bad[i].plen);
        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_IMAGE, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x50/状态 01（得到 cmd=%02X status=%02X）", bad[i].why,
                  s_reply[4], s_reply[7]);
        CHECK_MSG((s_render_calls == 0) && (s_slot_save_calls == 0),
                  "越界（%s）不得画/存（render=%d save=%d）", bad[i].why, s_render_calls,
                  s_slot_save_calls);
    }

    /* 几何 232×100 → 位图 29×100 = 2900 > LCSP_BITMAP_MAX_BYTES(2800)：明确拒绝 */
    stubs_reset();
    s_screen_rows      = 232U;
    s_screen_cols      = 100U;
    const uint8_t big4[4] = {1U, 1U, 0x00, 0x00};
    (void)cmd_run(APP_LCSP_CMD_SHOW_IMAGE, 0x0E10, big4, (uint16_t)sizeof(big4));
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_IMAGE, APP_LCSP_STATUS_FAIL),
              "位图超 LCSP_BITMAP_MAX_BYTES 应明确拒绝（得到 status=%02X）", s_reply[7]);
    CHECK_MSG((s_render_calls == 0) && (s_slot_save_calls == 0),
              "超上限时不得画/存（render=%d save=%d）", s_render_calls, s_slot_save_calls);
}

/** @brief 0x06 显示已存图片：位图**原样**喂画布（存的已是 MSB-first）+ 必须 persist
 *
 *  **为什么需要**：槽位里存的就是工程位序，出槽位时若再翻一次，屏上又变回镜像；
 *  `persist=true` 是"调图就是为了掉电后还在"（协议 7.6），漏了则重启回到上一张 ——
 * 现场表现是"换了图，停一次电又变回去"。 */
static void case_cmd_show_slot(void)
{
    TEST_BEGIN("0x06 显示已存图片：点阵原样不改、颜色取自槽位、persist=true");

    stubs_reset();

    static const uint8_t bm[2] = {0x81, 0x42};
    slot_seed(2, 8U, 2U, 3U /* 黄 */, bm, 2U);

    const uint8_t slot = 2U;
    uint16_t      n    = cmd_run(APP_LCSP_CMD_SHOW_SLOT, 0x0F01, &slot, 1);

    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_SLOT, APP_LCSP_STATUS_OK),
              "0x06 应回 0x60/状态 0（得到 cmd=%02X status=%02X）", s_reply[4], s_reply[7]);
    CHECK_MSG((s_render_calls == 2) && (s_rc0_type == APP_RENDER_TYPE_FILL) &&
                  (s_rc0_color == DEV_DISPLAY_COLOR_BLACK) && (s_rc0_w == 8U) &&
                  (s_rc0_h == 2U) && (s_rc_type == APP_RENDER_TYPE_BITMAP) && (s_rc_x == 0) &&
                  (s_rc_y == 0) && (s_rc_w == 8U) && (s_rc_h == 2U) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_YELLOW) && s_rc_persist,
              "应为 FILL(0,0,8,2,黑) + BITMAP(0,0,8,2,YELLOW) 且 persist=true"
              "（calls=%d 首调 type=%d color=%u persist=%d）",
              s_render_calls, (int)s_rc0_type, (unsigned)s_rc_color, (int)s_rc_persist);
    CHECK_MSG((s_rc_bitmap_len == 2U) && (s_rc_bitmap_buf[0] == 0x81) &&
                  (s_rc_bitmap_buf[1] == 0x42),
              "槽位点阵必须**原样**喂画布，不得再翻位序（得到 %02X %02X）",
              s_rc_bitmap_buf[0], s_rc_bitmap_buf[1]);
    CHECK_MSG((s_slot_save_calls == 0) && (n == 10U),
              "调图不得写槽位记录，应答 10 字节（save=%d len=%u）", s_slot_save_calls,
              (unsigned)n);
}

/** @brief 0x06 越界矩阵：槽位号 / 空槽位 / 几何不匹配 / 颜色非法 → 0x60 状态 01 且不画
 *
 *  **为什么需要**：几何必须与**当前**屏一致（存图后换屏/换级联配置都会变）。不匹配时
 *  按旧 stride 铺新屏在画面上是一幅斜纹，而 CRC / 长度全通过 —— 现场只会报"图不对"，
 *  查不到原因。空槽位则必须保持原画面（协议 7.6），不能顺手清屏。 */
static void case_cmd_show_slot_bounds(void)
{
    TEST_BEGIN("0x06 越界矩阵：槽位 0/4、空槽位、几何不匹配、颜色非法 → 状态 01 且不画");

    static const uint8_t bm[2] = {0xFF, 0x00};

    static const struct {
        uint8_t     slot;
        bool        seed;
        uint16_t    rows;
        uint16_t    cols;
        uint8_t     color;
        const char *why;
    } bad[] = {
        {0, false, 0, 0, 0, "槽位 0（仅显示，不是合法槽位）"},
        {4, false, 0, 0, 0, "槽位 4（超出 3 个）"},
        {3, false, 0, 0, 0, "空槽位"},
        {1, true, 16, 2, 1, "槽位几何 16×2 与当前 8×2 不符"},
        {1, true, 8, 2, 9, "槽位颜色 9 越界"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        if (bad[i].seed)
            slot_seed(bad[i].slot, bad[i].rows, bad[i].cols, bad[i].color, bm, 2U);

        const uint8_t slot = bad[i].slot;
        (void)cmd_run(APP_LCSP_CMD_SHOW_SLOT, 0x1000U + (uint16_t)i, &slot, 1);

        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SHOW_SLOT, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x60/状态 01（得到 cmd=%02X status=%02X）", bad[i].why,
                  s_reply[4], s_reply[7]);
        CHECK_MSG(s_render_calls == 0, "越界（%s）不得画（calls=%d）—— 原画面必须保持",
                  bad[i].why, s_render_calls);
    }
}

/* ================================================================
 *  命令层：0x07 亮度 / 0x08 显示开关
 * ================================================================ */

/** @brief 0x07 亮度：0–100 固定档（四舍五入）、0xFF 恢复自动跟随、且掉电保持
 *
 *  **为什么需要**：两条分支只差一个值（0xFF 是"自动"，不是"最暗"），写错就会
 *  "发自动调光 → 屏黑掉"。0% 必须映射到**档 1** 而不是 0：本工程档位 0 与"关屏"
 *  共用硬件口径，映射到 0 等于顺手把屏关了（协议 7.7 说的是"0 最暗"，关屏是 0x08
 *  的职责）。落盘是协议要求（7.7），不存则每次掉电回到出厂亮度。 */
static void case_cmd_brightness(void)
{
    TEST_BEGIN("0x07 亮度：0→档1 / 50→档4 / 100→档7 / 0xFF→自动跟随，且都落盘");

    static const struct {
        uint8_t value;
        bool    auto_mode;
        uint8_t level;
        const char *why;
    } cases[] = {
        {0, false, 1, "0%（协议最暗，映射到档 1 而不是关屏档 0）"},
        {50, false, 4, "50%（四舍五入到最近档）"},
        {100, false, 7, "100%（上限）"},
        {LCSP_BRIGHTNESS_AUTO, true, 0, "0xFF（自动光感）"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        stubs_reset();
        s_cfg_valid = true;
        const uint8_t v = cases[i].value;
        (void)cmd_run(APP_LCSP_CMD_SET_BRIGHT, 0x1100U + (uint16_t)i, &v, 1);

        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_BRIGHT, APP_LCSP_STATUS_OK),
                  "%s 应回 0x70/状态 0（得到 cmd=%02X status=%02X）", cases[i].why, s_reply[4],
                  s_reply[7]);
        CHECK_MSG(cases[i].auto_mode
                      ? ((s_resume_calls == 1) && (s_fixed_calls == 0))
                      : ((s_fixed_calls == 1) && (s_fixed_level == cases[i].level) &&
                         (s_resume_calls == 0)),
                  "%s 的亮度调用不对（fixed=%d level=%u resume=%d）", cases[i].why,
                  s_fixed_calls, (unsigned)s_fixed_level, s_resume_calls);
        CHECK_MSG((s_cfg_save_calls == 1) && (s_cfg_saved.brightness == cases[i].value),
                  "%s 必须落盘 brightness=%u（save=%d 值=%u）", cases[i].why,
                  (unsigned)cases[i].value, s_cfg_save_calls,
                  (unsigned)s_cfg_saved.brightness);
    }
}

/** @brief 0x07 越界矩阵：101..0xFE / 长度≠1 → 0x70 状态 01 且不动亮度、不落盘
 *
 *  **为什么需要**：101..0xFE 是未定义区间。若被钳到 100 或回绕，一条乱码命令就把现场
 *  调好的亮度改掉；而"先改亮度后校验"的实现会连 0x01 都回不出来就已经生效了。 */
static void case_cmd_brightness_bounds(void)
{
    TEST_BEGIN("0x07 越界矩阵：101 / 0xFE / 长度 0 与 2 → 0x70 状态 01 且零副作用");

    static const struct {
        uint16_t    len;
        uint8_t     value;
        const char *why;
    } bad[] = {
        {1, 101, "101（超出 0–100）"},
        {1, 0xFE, "0xFE（不是自动标志 0xFF）"},
        {0, 0, "没有 DATA"},
        {2, 50, "多一个字节"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        const uint8_t buf[2] = {bad[i].value, 0x00};
        (void)cmd_run(APP_LCSP_CMD_SET_BRIGHT, 0x1200U + (uint16_t)i, buf, bad[i].len);

        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_BRIGHT, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x70/状态 01（得到 cmd=%02X status=%02X）", bad[i].why,
                  s_reply[4], s_reply[7]);
        CHECK_MSG((s_fixed_calls == 0) && (s_resume_calls == 0) && (s_cfg_save_calls == 0),
                  "越界（%s）不得动亮度/落盘（fixed=%d resume=%d save=%d）", bad[i].why,
                  s_fixed_calls, s_resume_calls, s_cfg_save_calls);
    }
}

/** @brief 0x08 显示开关：只切 OE 输出、亮度期望值不动，且开关状态掉电保持
 *
 *  **为什么需要**：与 CDSTD A0 同一口径 —— 关屏**不是**设亮度 0（那会永久丢掉档位），
 *  所以本命令只能调 `app_screen_oe_*`，一次都不许碰光敏接口。开关落盘（协议 7.8）
 *  决定"掉电重启后屏是开是关"：不存的话维护现场关掉的屏会在重启后自己亮起来。 */
static void case_cmd_display_onoff(void)
{
    TEST_BEGIN("0x08 显示开关：0→撤输出 / 1→使能，均不动亮度、均落盘");

    stubs_reset();
    s_cfg_valid = true;

    const uint8_t off = 0U;
    uint16_t      n   = cmd_run(APP_LCSP_CMD_SET_OE, 0x1301, &off, 1);

    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_OE, APP_LCSP_STATUS_OK),
              "关屏应回 0x80/状态 0（得到 cmd=%02X status=%02X）", s_reply[4], s_reply[7]);
    CHECK_MSG((s_oe_disable_calls == 1) && (s_oe_enable_calls == 0) && (!s_oe_state),
              "关屏应撤掉输出（disable=%d enable=%d state=%d）", s_oe_disable_calls,
              s_oe_enable_calls, (int)s_oe_state);
    CHECK_MSG((s_cfg_save_calls == 1) && (s_cfg_saved.oe_enabled == 0U),
              "关屏状态必须落盘 oe_enabled=0（save=%d 值=%u）", s_cfg_save_calls,
              (unsigned)s_cfg_saved.oe_enabled);
    CHECK_MSG((s_fixed_calls == 0) && (s_resume_calls == 0),
              "开关不得动光敏（否则自动跟随下关屏就回不去了）");

    const uint8_t on = 1U;
    n = cmd_run(APP_LCSP_CMD_SET_OE, 0x1302, &on, 1);
    CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_OE, APP_LCSP_STATUS_OK) &&
                  (s_oe_enable_calls == 1) && s_oe_state,
              "开屏应使能输出（enable=%d state=%d status=%02X）", s_oe_enable_calls,
              (int)s_oe_state, s_reply[7]);
    CHECK_MSG((s_cfg_saved.oe_enabled == 1U) && (n == 10U), "开屏状态必须落盘 oe_enabled=1");
}

/** @brief 0x08 越界矩阵：取值 ≥2 / 长度≠1 → 0x80 状态 01 且零副作用
 *
 *  **为什么需要**：取值 2 若被当成"非 0 即开"，一条乱码就把维护现场关掉的屏点亮；
 *  长度≠1 时 DATA 可能是帧里的残留字节，照用等于随机开关屏。 */
static void case_cmd_display_onoff_bounds(void)
{
    TEST_BEGIN("0x08 越界矩阵：取值 2 / 0xFF / 长度 0 与 2 → 0x80 状态 01 且零副作用");

    static const struct {
        uint16_t    len;
        uint8_t     value;
        const char *why;
    } bad[] = {
        {1, 2, "取值 2"},
        {1, 0xFF, "取值 0xFF"},
        {0, 0, "没有 DATA"},
        {2, 1, "多一个字节"},
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stubs_reset();
        const uint8_t buf[2] = {bad[i].value, 0x00};
        (void)cmd_run(APP_LCSP_CMD_SET_OE, 0x1400U + (uint16_t)i, buf, bad[i].len);

        CHECK_MSG(reply_is_status(APP_LCSP_REPLY_SET_OE, APP_LCSP_STATUS_FAIL),
                  "越界（%s）应回 0x80/状态 01（得到 cmd=%02X status=%02X）", bad[i].why,
                  s_reply[4], s_reply[7]);
        CHECK_MSG((s_oe_enable_calls == 0) && (s_oe_disable_calls == 0) &&
                      (s_cfg_save_calls == 0),
                  "越界（%s）不得切输出/落盘（en=%d off=%d save=%d）", bad[i].why,
                  s_oe_enable_calls, s_oe_disable_calls, s_cfg_save_calls);
    }
}

/* ================================================================
 *  任务侧：分派 / 判重 / 未知命令 / 重启
 * ================================================================ */

/** @brief 任务贯通：真队列 + 真命令表 + 真处理函数，且**不看 aux**
 *
 *  **为什么需要**：任务从帧里的 CMD 字节分派（`frame->cmd & 0x0F` 查表），`msg->aux`
 *  是框架给"探针分类结果"的通道，本协议不使用。这里故意把 aux 填成 0xFF（一个既不
 *  等于功能号、也不在表内的值）：若实现哪天改成按 aux 分派，现场表现是"所有命令都被
 *  当未知命令回 0xF0"，而报文本身完全合法 —— 只能靠这条用例发现。 */
static void case_task_dispatch(void)
{
    TEST_BEGIN("任务：绑定两通道、真帧经真表分派（aux 不参与分派）");

    signal(SIGALRM, on_hang);
    alarm(20);

    stubs_reset();
    dup_reset();
    s_cfg_valid = false; /* 上电应用走"无记录"分支，不改 IP */

    _lcsp_module_init();
    s_lcsp_pcb.rb = &s_lcsp_rb; /* 探针用例借用过 pcb 的 rb 指针，这里还原 */

    CHECK_MSG(s_bind_calls == 2, "应绑定 TCP/UDP 两个通道（得到 %d）", s_bind_calls);
    CHECK_MSG(nullptr != s_lcsp_msg_queue, "帧队列应已建立");
    CHECK_MSG(nullptr != s_lcsp_task_handle, "协议任务应已创建");

    const uint8_t fill_red = 1U;
    uint16_t      n        = frame_make(s_frame, APP_LCSP_CMD_FILL_SCREEN, 0x1501, &fill_red, 1);

    tx_reset();
    render_reset();
    CHECK_MSG(task_feed(s_frame, n, 0xFFU), "投递失败（队列满？）");
    CHECK_MSG(wait_tx(1, 2000), "投递的帧未被分派（取帧/查表仍有问题）");
    CHECK_MSG((s_tx_len == 10) && (s_tx_buf[4] == APP_LCSP_REPLY_FILL_SCREEN) &&
                  (s_tx_buf[7] == APP_LCSP_STATUS_OK) && (s_tx_buf[2] == 0x15) &&
                  (s_tx_buf[3] == 0x01),
              "应答应为 0x40/状态 0 且 SEQ 回显 0x1501（cmd=%02X status=%02X seq=%02X%02X）",
              s_tx_buf[4], s_tx_buf[7], s_tx_buf[2], s_tx_buf[3]);
    CHECK_MSG((s_render_calls == 1) && (s_rc_type == APP_RENDER_TYPE_FILL) &&
                  (s_rc_color == DEV_DISPLAY_COLOR_RED) && (!s_rc_persist),
              "aux=0xFF 也必须按帧里的 CMD 分派并渲染 FILL(RED)（calls=%d color=%u）",
              s_render_calls, (unsigned)s_rc_color);
    CHECK_MSG(s_tx_ccb == &s_task_ccb, "应答应回到帧来源通道");
    CHECK_MSG(!s_tx_broadcast, "0x04 的应答不是广播");

    /* 0x03 的广播标志必须一路传到通道：命令层置位只是"意图"，任务不转发的话上位机在
       跨网段/NAT 时就收不到设备地址（协议 7.3 的发现功能整个失效）。 */
    n = frame_make(s_frame, APP_LCSP_CMD_QUERY_IP, 0x1502, nullptr, 0);
    CHECK_MSG(task_feed(s_frame, n, 3U) && wait_tx(2, 2000), "0x03 的帧未被分派");
    CHECK_MSG((s_tx_buf[4] == APP_LCSP_REPLY_QUERY_IP) && s_tx_broadcast,
              "0x03 的应答必须以广播标志发出（cmd=%02X broadcast=%d）", s_tx_buf[4],
              (int)s_tx_broadcast);

    alarm(0);
}

/** @brief 重复帧：业务动作只做一次，但**应答原样重发**；换 SEQ / 换 CMD 必须重新执行
 *
 *  **为什么需要**：上位机没收到应答会用同一个 SEQ 重发（协议 4.3.4）。不判重 →
 *  一幅图重存一次槽位（一次 64KB 扇区擦写）、重启命令被重发就重启两次；只丢不答 →
 *  上位机一直等不到应答，按 N1 判链路故障、反复断链重连（现场"时好时坏"）。
 *  判重键是 (通道, SEQ, CMD) —— 只有 SEQ 不够：上位机换连接或自己复位后从 0 重计数时，
 *  同一序号是完全不同的命令，所以换 SEQ / 换 CMD 都必须重新执行。 */
static void case_task_dup_frame(void)
{
    TEST_BEGIN("重复帧：同 SEQ+CMD 只执行一次但应答逐字节重发；换 SEQ/CMD 重新执行");

    signal(SIGALRM, on_hang);
    alarm(20);

    stubs_reset();
    dup_reset();

    if (nullptr == s_lcsp_msg_queue) {
        CHECK_MSG(0, "任务未初始化（见 case_task_dispatch）");
        alarm(0);
        return;
    }

    const uint8_t red    = 1U;
    uint16_t      n      = frame_make(s_frame, APP_LCSP_CMD_FILL_SCREEN, 0x1601, &red, 1);
    uint8_t       first[LCSP_REPLY_MAX_LEN];
    int           first_len = 0;

    /* 第 1 次：正常执行 */
    tx_reset();
    render_reset();
    CHECK_MSG(task_feed(s_frame, n, 4U), "第 1 帧投递失败");
    CHECK_MSG(wait_tx(1, 2000) && wait_dup_ready(0x1601, APP_LCSP_CMD_FILL_SCREEN, 1000),
              "第 1 帧未被分派（或判重缓存没记下来）");
    CHECK_MSG((s_render_calls == 1) && (s_tx_len == 10) &&
                  (s_tx_buf[4] == APP_LCSP_REPLY_FILL_SCREEN) &&
                  (s_tx_buf[7] == APP_LCSP_STATUS_OK),
              "第 1 帧应执行一次并回 0x40/状态 0（render=%d cmd=%02X status=%02X）",
              s_render_calls, s_tx_buf[4], s_tx_buf[7]);
    first_len = s_tx_len;
    memcpy(first, s_tx_buf, sizeof(first));

    /* 第 2 次：逐字节相同的帧（同一 SEQ+CMD）→ 业务不重做，应答原样再发 */
    CHECK_MSG(task_feed(s_frame, n, 4U), "重传帧投递失败");
    CHECK_MSG(wait_tx(2, 2000), "重传帧没有回任何应答（只丢不答会让上位机判链路故障）");
    CHECK_MSG(s_render_calls == 1, "重复帧不得重复执行业务动作（render=%d 次）", s_render_calls);
    CHECK_MSG((s_tx_len == first_len) && (0 == memcmp(s_tx_buf, first, (size_t)first_len)),
              "重复帧的应答必须与上次**逐字节相同**（len %d → %d）", first_len, s_tx_len);

    /* 换 SEQ：上位机复位后重新计数，同一 CMD 完全可能是新命令 → 必须重新执行 */
    n = frame_make(s_frame, APP_LCSP_CMD_FILL_SCREEN, 0x1602, &red, 1);
    CHECK_MSG(task_feed(s_frame, n, 4U) && wait_tx(3, 2000), "换 SEQ 的帧未被分派");
    CHECK_MSG(s_render_calls == 2 && wait_dup_ready(0x1602, APP_LCSP_CMD_FILL_SCREEN, 1000),
              "换 SEQ 必须重新执行（render=%d 次）", s_render_calls);

    /* 换 CMD、SEQ 不变：另一条命令 → 也必须重新执行（判重键含 CMD） */
    const uint8_t on = 1U;
    n = frame_make(s_frame, APP_LCSP_CMD_SET_OE, 0x1602, &on, 1);
    CHECK_MSG(task_feed(s_frame, n, 8U) && wait_tx(4, 2000), "换 CMD 的帧未被分派");
    CHECK_MSG((s_oe_enable_calls == 1) && (s_tx_buf[4] == APP_LCSP_REPLY_SET_OE),
              "换 CMD（SEQ 不变）必须重新执行（oe_enable=%d cmd=%02X）", s_oe_enable_calls,
              s_tx_buf[4]);

    alarm(0);
}

/** @brief 未定义请求码：回 0xF0 并**回显原命令字**
 *
 *  **为什么需要**：0x0A–0x0F 是未分配功能号，0x13 的低半字节虽是 3 但高半字节非 0
 *  （不是合法请求码）。协议 4.2 要求回通用错误并回显原码：回显是现场唯一能定位
 *  "是哪一个码没实现"的信息。静默丢弃则上位机只能靠超时，误判为链路故障。 */
static void case_task_unknown_cmd(void)
{
    TEST_BEGIN("未知命令：0x0A / 0x13 → 0xF0，DATA={01, 原命令字}");

    signal(SIGALRM, on_hang);
    alarm(20);

    stubs_reset();
    dup_reset();

    if (nullptr == s_lcsp_msg_queue) {
        CHECK_MSG(0, "任务未初始化（见 case_task_dispatch）");
        alarm(0);
        return;
    }

    static const struct {
        uint8_t     cmd;
        uint16_t    seq;
        const char *why;
    } unknown[] = {
        {0x0A, 0x1701, "0x0A（功能号超出命令表）"},
        {0x13, 0x1702, "0x13（高半字节非 0，不是合法请求码）"},
    };

    for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        uint16_t n = frame_make(s_frame, unknown[i].cmd, unknown[i].seq, nullptr, 0);
        CHECK_MSG(task_feed(s_frame, n, unknown[i].cmd) && wait_tx((int)i + 1, 2000),
                  "%s 没有应答", unknown[i].why);
        CHECK_MSG((s_tx_len == 11) && (s_tx_buf[4] == APP_LCSP_CMD_ERROR_REPLY) &&
                      (s_tx_buf[5] == 0U) && (s_tx_buf[6] == 2U) &&
                      (s_tx_buf[7] == APP_LCSP_STATUS_FAIL) && (s_tx_buf[8] == unknown[i].cmd),
                  "%s 应回 0xF0/状态 01 并回显 %02X（得到 cmd=%02X len=%u data=%02X）",
                  unknown[i].why, unknown[i].cmd, s_tx_buf[4], (unsigned)s_tx_len, s_tx_buf[8]);
    }

    alarm(0);
}

/** @brief 0x09 重启：先回应答、**再**延时复位（顺序可观测）
 *
 *  **为什么需要**：协议 7.9.1 要求应答发出后 ≥200ms 才复位。顺序反了（先复位）应答
 *  还没从 LwIP 出去，上位机永远等不到 0x90，只能按"链路故障"处理 —— 现场看到的是
 *  "发了重启命令，设备没反应"（其实它已经重启完了）。这里用事件序号证明顺序，
 *  不是只看"两个都被调过"。 */
static void case_cmd_reboot(void)
{
    TEST_BEGIN("0x09 重启：回 0x90 状态 0，复位恰好一次且**在应答之后**");

    signal(SIGALRM, on_hang);
    alarm(20);

    stubs_reset();
    dup_reset();

    if (nullptr == s_lcsp_msg_queue) {
        CHECK_MSG(0, "任务未初始化（见 case_task_dispatch）");
        alarm(0);
        return;
    }

    /* 现状哨兵：0x09 **刻意不校验 DATA 长度** —— 重启是远程故障恢复的最后手段，
       因为多带了一个字节就拒绝执行，现场就只能跑去现场按复位键。多带一个字节
       同样必须回应答并复位。 */
    const uint8_t extra = 0x5A;
    uint16_t      n     = frame_make(s_frame, APP_LCSP_CMD_REBOOT, 0x1801, &extra, 1);

    CHECK_MSG(task_feed(s_frame, n, 9U), "重启帧投递失败");
    CHECK_MSG(wait_tx(1, 2000), "重启命令没有回应答（上位机会一直等 0x90）");
    CHECK_MSG((s_tx_len == 10) && (s_tx_buf[4] == APP_LCSP_REPLY_REBOOT) &&
                  (s_tx_buf[7] == APP_LCSP_STATUS_OK) && (s_tx_buf[2] == 0x18) &&
                  (s_tx_buf[3] == 0x01),
              "应答应为 0x90/状态 0 且 SEQ 回显 0x1801（cmd=%02X status=%02X）", s_tx_buf[4],
              s_tx_buf[7]);

    CHECK_MSG(wait_reset(1, 3000), "应答之后应在 300ms 内复位（实际复位 %d 次）", s_reset_calls);
    CHECK_MSG(s_reset_calls == 1, "复位必须恰好一次（得到 %d 次）", s_reset_calls);
    CHECK_MSG(s_reset_event > s_tx_event,
              "必须先答后复位（tx_event=%d reset_event=%d；先复位会丢掉这条应答）", s_tx_event,
              s_reset_event);

    alarm(0);
}

/* ================================================================
 *  模块上电应用配置
 * ================================================================ */

/** @brief 上电应用网络：**无记录只落端口、不动 IP**；有记录则 IP 与端口都按记录
 *
 *  **为什么需要**：这两个分支的取舍是现场安全决定的。无记录时若把 IP 改成协议出厂值
 *  （192.168.1.100），上位机可能已经按板上默认地址连上了 —— 改完当场失联，而现场没有
 *  任何手段再连回来。端口相反：本协议靠 10028 被找到，出厂必须落这个值。
 *  有记录时端口为 0 表示"未设置"（旧记录/坏记录），此时不能把监听口设成 0（随机端口）。 */
static void case_boot_apply_net(void)
{
    TEST_BEGIN("上电应用：无记录只落端口不动 IP；有记录按记录应用 IP+端口");

    stubs_reset();
    s_cfg_valid = false;

    _lcsp_apply_cfg();

    CHECK_MSG((s_tcp_port_set == LCSP_PORT_DEFAULT) && (s_udp_port_set == LCSP_PORT_DEFAULT),
              "无记录时 TCP/UDP 端口都应落到 %u（得到 %u / %u）", (unsigned)LCSP_PORT_DEFAULT,
              (unsigned)s_tcp_port_set, (unsigned)s_udp_port_set);
    CHECK_MSG(s_net_set_calls == 0,
              "无记录时**不得**改 IP（协议出厂地址会让已连上的上位机当场失联）");

    /* 有效记录：IP/掩码/网关 + 端口（大端字节对）都应应用 */
    stubs_reset();
    s_cfg_valid = true;
    static const uint8_t ip[4]   = {192, 168, 0, 220};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4]   = {192, 168, 0, 1};
    memcpy(s_cfg.ip, ip, 4);
    memcpy(s_cfg.mask, mask, 4);
    memcpy(s_cfg.gw, gw, 4);
    s_cfg.port[0]    = 0x27;
    s_cfg.port[1]    = 0x1C; /* 10012：与默认 10028 不同，才能证明用的是记录值 */
    s_cfg.brightness = LCSP_BRIGHTNESS_AUTO;
    s_cfg.oe_enabled = 1U;

    _lcsp_apply_cfg();

    CHECK_MSG((s_net_set_calls == 1) && (0 == memcmp(s_net_ip, ip, 4)) &&
                  (0 == memcmp(s_net_mask, mask, 4)) && (0 == memcmp(s_net_gw, gw, 4)),
              "有效记录应把 IP/掩码/网关原样应用（调用 %d 次）", s_net_set_calls);
    CHECK_MSG((s_tcp_port_set == 10012U) && (s_udp_port_set == 10012U),
              "TCP/UDP 端口都应按记录的大端字节对解码为 10012（得到 %u / %u）",
              (unsigned)s_tcp_port_set, (unsigned)s_udp_port_set);
}

/** @brief 上电应用显示：亮度 0xFF→自动跟随、0–100→固定档、oe_enabled=0→关屏
 *
 *  **为什么需要**：上电与运行期必须走**同一组 API**（0x07/0x08 用的那两条），否则
 *  两套口径会漂移 —— 表现为"上电后亮度是对的，发一次命令就变了"，或"记录里关着屏，
 *  重启后自己亮起来"。0xFF 走 resume 而不是 set_fixed(0)：后者等于上电就把屏关掉。 */
static void case_boot_apply_display(void)
{
    TEST_BEGIN("上电应用显示：0xFF→光敏跟随 / 40→固定档 3 / oe=0→关屏");

    /* 自动 + 屏开 */
    stubs_reset();
    s_cfg_valid      = true;
    s_cfg.brightness = LCSP_BRIGHTNESS_AUTO;
    s_cfg.oe_enabled = 1U;
    _lcsp_apply_display_cfg(&s_cfg);

    CHECK_MSG((s_resume_calls == 1) && (s_fixed_calls == 0),
              "0xFF 应恢复光敏跟随（resume=%d fixed=%d）", s_resume_calls, s_fixed_calls);
    CHECK_MSG(s_oe_disable_calls == 0, "oe_enabled=1 时不得关屏（disable=%d）", s_oe_disable_calls);

    /* 固定 40%（→ 档 3）+ 屏关 */
    stubs_reset();
    s_cfg_valid      = true;
    s_cfg.brightness = 40U;
    s_cfg.oe_enabled = 0U;
    _lcsp_apply_display_cfg(&s_cfg);

    CHECK_MSG((s_fixed_calls == 1) && (s_fixed_level == app_lcsp_level_from_percent(40U)),
              "40%% 应落实固定档 %u（得到 fixed=%d level=%u）",
              (unsigned)app_lcsp_level_from_percent(40U), s_fixed_calls, (unsigned)s_fixed_level);
    CHECK_MSG((s_fixed_level == 3U) && (s_resume_calls == 0),
              "40%% 的固定档应为 3 且不得调 resume（得到 level=%u resume=%d）",
              (unsigned)s_fixed_level, s_resume_calls);
    CHECK_MSG((s_oe_disable_calls == 1) && (!s_oe_state),
              "oe_enabled=0 必须关屏（disable=%d state=%d）", s_oe_disable_calls,
              (int)s_oe_state);

    /* 0% 只能落到档 1：本工程档 0 与"关屏"共用硬件口径，落到 0 等于上电关屏 */
    stubs_reset();
    s_cfg_valid      = true;
    s_cfg.brightness = 0U;
    s_cfg.oe_enabled = 1U;
    _lcsp_apply_display_cfg(&s_cfg);
    CHECK_MSG((s_fixed_calls == 1) && (s_fixed_level == 1U),
              "0%% 应落到档 1 而不是 0（得到 level=%u；档 0 = 关屏）", (unsigned)s_fixed_level);
    CHECK_MSG(s_oe_disable_calls == 0, "亮度 0 不得顺带关屏（关屏是 0x08 的职责）");
}

/* ================================================================ */

int main(void)
{
    printf("LCSP 协议 host 单测\n");

    case_crc_sentinel();
    case_build_frame_golden();
    case_probe_header_and_short();
    case_probe_partial_frame();
    case_probe_crc_fail();
    case_probe_ready_contract();
    case_probe_skip_scratch();
    case_probe_no_cross_poison();
    case_cmd_device_info();
    case_cmd_device_info_state();
    case_cmd_device_info_golden_a17();
    case_cmd_set_ip();
    case_cmd_set_ip_bounds();
    case_cmd_query_ip();
    case_cmd_fill();
    case_cmd_fill_bounds();
    case_cmd_show_image();
    case_cmd_show_image_bounds();
    case_cmd_show_slot();
    case_cmd_show_slot_bounds();
    case_cmd_brightness();
    case_cmd_brightness_bounds();
    case_cmd_display_onoff();
    case_cmd_display_onoff_bounds();
    case_task_dispatch();
    case_task_dup_frame();
    case_task_unknown_cmd();
    case_cmd_reboot();
    case_boot_apply_net();
    case_boot_apply_display();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
