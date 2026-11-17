/**
 * @file    test_ah_tcp.c
 * @brief   AH/TCP 参数口：探针四态、0AH 设置/合并/静默丢弃、0BH 采集应答
 *
 * **为什么需要这个测试**：参数口是平台改设备 IP/MQTT 凭据的唯一入口，
 * 判错一个分支的症状都在现场：探针把半帧当整帧 → 配置帧被切断；
 * 模块类型判错 → 别的设备型号的参数被本机吞下；校验/落盘失败却回了成功 →
 * 平台以为改好了、重启后设备失联。这些都不能靠“上机抓一次包”，所以在 host 上钉住。
 *
 * 手法：用例 TU-include 生产 TU `app_ah_tcp.c`（0AH/0BH 处理函数与组帧是 static），
 * 外部依赖（配置层、RTC、网络、TCP 服务实例）全部用记录型桩。
 * 探针走真实的 `rb_write`/`rb_peek`，不手搓 pcb 内部状态。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"

#include "app_ah_cfg.h"
#include "app_ah_tcp.h"
#include "app_dispatch.h"
#include "app_tcp_server.h"
#include "board.h"
#include "crc_utils.h"
#include "ring_buffer.h"

/* 生产实现 TU-include：`_ah_tcp_cmd_set/get`、`_ah_tcp_send` 都是 static */
#include "../Application/Src/AH/app_ah_tcp.c"

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
 *  桩：配置层（RAM 镜像 + 落盘结果可注入）
 * ================================================================ */

static app_ah_cfg_t s_cfg;
static int          s_save_calls;
static int32_t      s_save_rc;
static bool         s_validate_ok;

void app_ah_cfg_get(app_ah_cfg_t *out)
{
    if (out != nullptr)
        *out = s_cfg;
}

bool app_ah_cfg_validate(const app_ah_cfg_t *cfg, app_ah_err_t *err)
{
    (void)cfg;
    if (err != nullptr)
        *err = s_validate_ok ? APP_AH_ERR_OK : APP_AH_ERR_PARAM;
    return s_validate_ok;
}

int32_t app_ah_cfg_save(const app_ah_cfg_t *cfg)
{
    s_save_calls++;
    if (s_save_rc != 0)
        return s_save_rc;
    if (cfg != nullptr)
        s_cfg = *cfg;
    return 0;
}

/* ================================================================
 *  桩：RTC / 网络 / 任务 / TCP 实例 / 分发绑定
 * ================================================================ */

static int      s_rtc_calls;
static uint32_t s_rtc_ts;
static int      s_net_set_calls;

pl_rtc_handle_t pl_rtc_get_handle(void)
{
    return nullptr;
}

bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts)
{
    (void)h;
    s_rtc_calls++;
    s_rtc_ts = ts;
    return true;
}

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memset(ip, 0, 4);
    memset(mask, 0, 4);
    memset(gw, 0, 4);
}

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    (void)ip;
    (void)mask;
    (void)gw;
    s_net_set_calls++;
}

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    return nullptr;
}

app_tcp_server_t *app_tcp_server_open(const char *name, uint16_t port, uint32_t idle_timeout_ms)
{
    (void)name;
    (void)port;
    (void)idle_timeout_ms;
    return nullptr;
}

osThreadId_t app_tcp_server_start_instance(app_tcp_server_t *srv)
{
    (void)srv;
    return nullptr;
}

app_ccb_t *app_tcp_server_ccb_of(const app_tcp_server_t *srv)
{
    (void)srv;
    return nullptr;
}

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
}

/* ================================================================
 *  桩：发送通道（记录最近一帧）
 * ================================================================ */

#define TX_MAX (AH_TCP_FRAME_MAX)

static uint8_t  s_tx[TX_MAX];
static uint16_t s_tx_len;
static int      s_tx_calls;

static app_ccb_t s_ccb = {.name = "ah_tcp_ut"};

int32_t app_ccb_send_to(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    (void)dst;
    s_tx_calls++;
    if (len > sizeof(s_tx))
        len = (uint16_t)sizeof(s_tx);
    memcpy(s_tx, data, len);
    s_tx_len = len;
    return (int32_t)len;
}

/* ================================================================
 *  探针夹具：真实 ring buffer
 * ================================================================ */

static uint8_t       s_rb_data[2048];
static ring_buffer_t s_rb = {
    .data = s_rb_data,
    .size = sizeof(s_rb_data),
};

static app_pcb_t s_pcb = {
    .name        = "ah_tcp_ut",
    .rb          = &s_rb,
    .payload_max = AH_TCP_FRAME_MAX,
};

static uint8_t s_scratch[FRAME_DATA_MAX_LEN];

static void _feed(const uint8_t *bytes, uint16_t len)
{
    rb_flush(&s_rb, nullptr);
    rb_write(&s_rb, bytes, len, nullptr);
}

/* ================================================================
 *  帧构造
 * ================================================================ */

static uint16_t _build_frame(uint8_t *out, uint8_t seq, const uint8_t *data, uint16_t len)
{
    out[0] = 0xFFU;
    out[1] = 0xFFU;
    out[2] = AH_TCP_VER;
    out[3] = seq;
    out[4] = 0U;
    out[5] = 0U;
    out[6] = (uint8_t)(len >> 8);
    out[7] = (uint8_t)len;
    memcpy(out + 8U, data, len);

    uint16_t crc = crc16_xmodem(out + 2U, (size_t)(6U + len));
    out[8U + len] = (uint8_t)(crc >> 8);
    out[9U + len] = (uint8_t)crc;
    return (uint16_t)(AH_TCP_FRAME_OVERHEAD + len);
}

static void _put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void _put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 读大端复用生产 TU 的 `_rd16`/`_rd32`（TU-include 后已在作用域内） */

/* ================================================================
 *  配置夹具
 * ================================================================ */

static const uint8_t k_lane[5] = {0x52U, 0x01U, 0x13U, 0x14U, 0x01U};
static const uint8_t k_cert[8] = {0xC0U, 0xC1U, 0xC2U, 0xC3U, 0xC4U, 0xC5U, 0xC6U, 0xC7U};

static app_ah_cfg_t _base_cfg(void)
{
    app_ah_cfg_t c;
    memset(&c, 0, sizeof(c));

    memcpy(c.lane_hex, k_lane, sizeof(c.lane_hex));
    memcpy(c.cert, k_cert, sizeof(c.cert));

    const uint8_t s1[4] = {192U, 168U, 1U, 100U};
    const uint8_t s2[4] = {192U, 168U, 1U, 101U};
    const uint8_t s3[4] = {192U, 168U, 1U, 102U};
    memcpy(c.server1_ip, s1, 4);
    c.server1_port = 6000U;
    memcpy(c.server2_ip, s2, 4);
    c.server2_port = 6001U;
    memcpy(c.server3_ip, s3, 4);
    c.server3_port = 6002U;

    const uint8_t mq[4] = {120U, 46U, 136U, 199U};
    memcpy(c.mqtt_ip, mq, 4);
    c.mqtt_port = 6000U;
    memcpy(c.mqtt_user, "pxh", 3);
    memcpy(c.mqtt_pwd, "pw", 2);
    c.mqtt_qos = 0U;

    const uint8_t ntp[4] = {203U, 107U, 6U, 88U};
    memcpy(c.ntp_ip, ntp, 4);

    const uint8_t cip[4] = {192U, 168U, 1U, 50U};
    const uint8_t gw[4]  = {192U, 168U, 1U, 1U};
    const uint8_t mask[4] = {255U, 255U, 255U, 0U};
    memcpy(c.client_ip, cip, 4);
    memcpy(c.gateway, gw, 4);
    memcpy(c.netmask, mask, 4);
    c.client_port = 6001U;

    c.device_id  = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    c.device_no  = 1U;
    c.font_line  = 0U;
    return c;
}

static void _reset(void)
{
    s_cfg          = _base_cfg();
    s_save_calls   = 0;
    s_save_rc      = 0;
    s_validate_ok  = true;
    s_tx_calls     = 0;
    s_tx_len       = 0;
    s_rtc_calls    = 0;
    s_rtc_ts       = 0;
    s_net_set_calls = 0;
    memset(s_tx, 0, sizeof(s_tx));
    rb_flush(&s_rb, nullptr);
}

/** @brief 组一帧 0AH 请求：公共块清零、CmdType/ModeCnt 置位，尾部模块留空 */
static void _set_req(uint8_t *d, uint8_t mode)
{
    memset(d, 0, AH_TCP_FIXED_LEN + 3U);
    d[AH_TCP_OFF_CMD]      = AH_TCP_CMD_SET;
    d[AH_TCP_OFF_MODE_CNT] = mode;
}

/* ================================================================
 *  探针
 * ================================================================ */

/** @brief 不足 8 字节：必须 WAIT，不能把半截帧头当垃圾丢掉 */
static void case_probe_wait_head(void)
{
    TEST_BEGIN("探针：不足 8 字节 → WAIT");

    _reset();

    uint8_t f[64] = {0};
    uint16_t n     = _build_frame(f, 0x01U, k_lane, 5U);
    _feed(f, 7U);

    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    app_pcb_probe_state_t st =
        app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);

    CHECK_MSG(st == APP_PCB_PROBE_STATE_WAIT, "7 字节时状态应 WAIT，实际 %d", (int)st);
    CHECK_MSG(n == 15U, "夹具自检：帧长应为 15");
}

/** @brief 帧头不对：必须 FAKE，让框架逐字节重扫找下一个 FF FF */
static void case_probe_fake_head(void)
{
    TEST_BEGIN("探针：帧头/长度非法 → FAKE");

    _reset();

    uint8_t f[16] = {0};
    f[0]          = 0xFEU;
    f[1]          = 0xFEU;
    _feed(f, sizeof(f));

    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    app_pcb_probe_state_t st =
        app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_FAKE, "帧头不是 FF FF 应 FAKE，实际 %d", (int)st);

    /* LEN = 0 */
    memset(f, 0, sizeof(f));
    f[0] = 0xFFU;
    f[1] = 0xFFU;
    _feed(f, sizeof(f));
    st = app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_FAKE, "LEN=0 应 FAKE，实际 %d", (int)st);

    /* LEN = 129 > AH_TCP_DATA_MAX */
    _put16(f + 6, 129U);
    _feed(f, sizeof(f));
    st = app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_FAKE, "LEN=129 应 FAKE，实际 %d", (int)st);
}

/** @brief 版本/保留字节不认识：按 LEN 整帧 SKIP，不逐字节重扫 */
static void case_probe_skip_unknown_version(void)
{
    TEST_BEGIN("探针：VER/RSV 不认识 → SKIP（整帧跳过）");

    _reset();

    uint8_t  f[64] = {0};
    uint16_t n     = _build_frame(f, 0x01U, k_lane, 5U);

    f[2] = 0x01U;
    _feed(f, n);

    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    app_pcb_probe_state_t st =
        app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_SKIP, "VER=1 应 SKIP，实际 %d", (int)st);
    CHECK_MSG(total == n, "SKIP 应给出整帧长度 %u，实际 %u", (unsigned)n, (unsigned)total);

    f[2] = AH_TCP_VER;
    f[4] = 0x5AU;
    _feed(f, n);
    st = app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_SKIP, "RSV 非 0 应 SKIP，实际 %d", (int)st);
}

/** @brief CRC 错：FAKE，不能投给任务 */
static void case_probe_fake_crc(void)
{
    TEST_BEGIN("探针：CRC 错 → FAKE");

    _reset();

    uint8_t  f[64] = {0};
    uint16_t n     = _build_frame(f, 0x01U, k_lane, 5U);
    f[9] ^= 0x5AU; /* 动 DATA */

    _feed(f, n);

    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    app_pcb_probe_state_t st =
        app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_FAKE, "CRC 错应 FAKE，实际 %d", (int)st);
}

/** @brief 完整帧：READY、total 正确、aux = CmdType */
static void case_probe_ready(void)
{
    TEST_BEGIN("探针：完整帧 → READY（total/aux 正确）");

    _reset();

    uint8_t  data[AH_TCP_0B_REQ_LEN] = {0};
    uint8_t  f[64]                   = {0};
    uint16_t n;

    data[AH_TCP_OFF_CMD] = AH_TCP_CMD_GET;
    n                    = _build_frame(f, 0x02U, data, sizeof(data));

    _feed(f, (uint16_t)(n - 1U));
    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    app_pcb_probe_state_t st =
        app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_WAIT, "差 1 字节应 WAIT，实际 %d", (int)st);

    _feed(f, n);
    st = app_ah_tcp_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_READY, "完整帧应 READY，实际 %d", (int)st);
    CHECK_MSG(total == n, "total 应为 %u，实际 %u", (unsigned)n, (unsigned)total);
    CHECK_MSG(aux == AH_TCP_CMD_GET, "aux 应为 0x0B，实际 0x%02X", (unsigned)aux);
}

/* ================================================================
 *  0AH 设置
 * ================================================================ */

/** @brief 合法 0AH：字段合并、落盘、RTC 校时、应答 21 B（ErrCode=0） */
static void case_set_ok(void)
{
    TEST_BEGIN("0AH：合法参数 → 落盘 + 校时 + 应答");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 1U);
    _put32(d + AH_TCP_OFF_UNIX_TM, 1741183200U);

    const uint8_t lane[5] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0x02U};
    const uint8_t cert[8] = {0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U};
    memcpy(d + AH_TCP_OFF_LANE, lane, sizeof(lane));
    memcpy(d + AH_TCP_OFF_CERT, cert, sizeof(cert));

    const uint8_t mq[4] = {172U, 16U, 0U, 1U};
    memcpy(d + AH_TCP_OFF_MQTT_IP, mq, 4);
    _put16(d + AH_TCP_OFF_MQTT_PORT, 1883U);
    memcpy(d + AH_TCP_OFF_MQTT_USER, "mqttuser", 8);
    memcpy(d + AH_TCP_OFF_MQTT_PWD, "secret", 6);
    d[AH_TCP_OFF_MQTT_QOS] = 1U;

    const uint8_t cip[4] = {10U, 10U, 10U, 10U};
    const uint8_t gw[4]  = {10U, 10U, 10U, 1U};
    const uint8_t mask[4] = {255U, 255U, 255U, 0U};
    memcpy(d + AH_TCP_OFF_CLIENT_IP, cip, 4);
    _put16(d + AH_TCP_OFF_CLIENT_PORT, 6002U);
    memcpy(d + AH_TCP_OFF_GATEWAY, gw, 4);
    memcpy(d + AH_TCP_OFF_NETMASK, mask, 4);

    d[AH_TCP_OFF_MODULES]     = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    d[AH_TCP_OFF_MODULES + 1] = 0x07U;

    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN + 2U);

    CHECK_MSG(s_save_calls == 1, "应落盘一次，实际 %d", s_save_calls);
    CHECK_MSG(s_tx_calls == 1, "应应答一帧，实际 %d", s_tx_calls);
    CHECK_MSG(s_tx_len == (8U + AH_TCP_0A_RSP_LEN + 2U), "0AH 应答帧长应为 31，实际 %u",
              (unsigned)s_tx_len);
    CHECK_MSG(s_tx[3] == 0x10U, "应答 SEQ 应为 0x10，实际 0x%02X", (unsigned)s_tx[3]);
    CHECK_MSG(s_tx[8] == AH_TCP_CMD_SET, "应答 CmdType 应为 0x0A");
    CHECK_MSG(s_tx[8U + AH_TCP_0A_OFF_ERR] == 0U, "ErrCode 应为 0，实际 %u",
              (unsigned)s_tx[8U + AH_TCP_0A_OFF_ERR]);

    CHECK_MSG(memcmp(s_cfg.lane_hex, lane, sizeof(lane)) == 0, "LaneHex 应被更新");
    CHECK_MSG(memcmp(s_cfg.cert, cert, sizeof(cert)) == 0, "Cert 应被保存（只存不校验）");
    CHECK_MSG(memcmp(s_cfg.mqtt_ip, mq, 4) == 0, "MQTTIP 应被更新");
    CHECK_MSG(s_cfg.mqtt_port == 1883U, "MQTTPort 应为 1883，实际 %u", (unsigned)s_cfg.mqtt_port);
    CHECK_MSG(memcmp(s_cfg.mqtt_user, "mqttuser", 8) == 0, "MQTTUser 应被更新");
    CHECK_MSG(memcmp(s_cfg.mqtt_pwd, "secret", 6) == 0, "MQTTPwd 应被更新");
    CHECK_MSG(s_cfg.mqtt_qos == 1U, "MQTTQOS 应为 1");
    CHECK_MSG(memcmp(s_cfg.client_ip, cip, 4) == 0, "ClientIP 应被更新");
    CHECK_MSG(s_cfg.client_port == 6002U, "ClientPort 应为 6002（重启生效）");
    CHECK_MSG(s_cfg.device_no == 0x07U, "DeviceNo 应更新为 7");
    CHECK_MSG(s_rtc_calls == 1 && s_rtc_ts == 1741183200U, "成功后应写一次 RTC（时间戳透传）");
    CHECK_MSG(s_net_set_calls == 0, "0AH 不得立即改运行态网络（重启生效）");
}

/** @brief 全 0 字段 = 保持：84 B、ModeCnt=0 的帧不应改动任何配置 */
static void case_set_zero_keeps(void)
{
    TEST_BEGIN("0AH：全 0 字段保持原值（ModeCnt=0）");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 0U);

    app_ah_cfg_t before = s_cfg;
    _ah_tcp_cmd_set(&s_ccb, 0x00U, d, AH_TCP_FIXED_LEN);

    CHECK_MSG(s_save_calls == 1, "ModeCnt=0 也应走一次保存，实际 %d", s_save_calls);
    CHECK_MSG(memcmp(&s_cfg, &before, sizeof(before)) == 0, "全 0 帧不应改动配置");
    CHECK_MSG(s_tx_calls == 1, "应应答一帧，实际 %d", s_tx_calls);
    CHECK_MSG(s_tx[3] == 0x10U, "请求 SEQ 低半字节为 0 时应答 SEQ 应回落 0x10");
    CHECK_MSG(s_rtc_calls == 0, "UnixTm=0 不应写 RTC");
}

/** @brief 模块表不符：整帧静默丢弃 —— 不应答、不落盘 */
static void case_set_module_silent_drop(void)
{
    TEST_BEGIN("0AH：模块类型/个数/长度不符 → 静默丢弃");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];

    /* 别的设备型号（12H） */
    _set_req(d, 1U);
    d[AH_TCP_OFF_MODULES]     = 0x12U;
    d[AH_TCP_OFF_MODULES + 1] = 0x01U;
    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN + 2U);
    CHECK_MSG(s_tx_calls == 0, "非本机模块类型不应应答，实际发了 %d 帧", s_tx_calls);
    CHECK_MSG(s_save_calls == 0, "非本机模块类型不应落盘");

    /* ModeCnt=2：超出本机能力 */
    _set_req(d, 2U);
    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN);
    CHECK_MSG(s_tx_calls == 0, "ModeCnt=2 不应应答");

    /* ModeCnt=1 但缺模块头 */
    _set_req(d, 1U);
    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN);
    CHECK_MSG(s_tx_calls == 0, "ModeCnt=1 缺模块头不应应答");

    /* 32H 模块长度应为 2，给 1 字节 */
    _set_req(d, 1U);
    d[AH_TCP_OFF_MODULES] = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN + 1U);
    CHECK_MSG(s_tx_calls == 0, "模块长度不符不应应答");
    CHECK_MSG(s_save_calls == 0, "静默丢弃全程不应落盘");
}

/** @brief UnixTm 越界：ErrCode=01，不落盘；边界值 0 表示“不改时间” */
static void case_set_bad_unix_time(void)
{
    TEST_BEGIN("0AH：UnixTm 越界 → ErrCode=01，不落盘");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 0U);
    _put32(d + AH_TCP_OFF_UNIX_TM, 100U);

    app_ah_cfg_t before = s_cfg;
    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, sizeof(d));

    CHECK_MSG(s_tx_calls == 1, "应应答一帧");
    CHECK_MSG(s_tx[8U + AH_TCP_0A_OFF_ERR] == (uint8_t)APP_AH_ERR_PARAM, "ErrCode 应为 01，实际 %u",
              (unsigned)s_tx[8U + AH_TCP_0A_OFF_ERR]);
    CHECK_MSG(s_save_calls == 0, "时间非法不应落盘");
    CHECK_MSG(memcmp(&s_cfg, &before, sizeof(before)) == 0, "时间非法不应改配置");
}

/** @brief 长度不足 84：ErrCode=05（格式错），不落盘 */
static void case_set_bad_format(void)
{
    TEST_BEGIN("0AH：固定部分不足 84 B → ErrCode=05");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 0U);

    _ah_tcp_cmd_set(&s_ccb, 0x01U, d, AH_TCP_FIXED_LEN - 1U);

    CHECK_MSG(s_tx_calls == 1, "应应答一帧");
    CHECK_MSG(s_tx[8U + AH_TCP_0A_OFF_ERR] == (uint8_t)APP_AH_ERR_FORMAT, "ErrCode 应为 05，实际 %u",
              (unsigned)s_tx[8U + AH_TCP_0A_OFF_ERR]);
    CHECK_MSG(s_save_calls == 0, "格式错不应落盘");
}

/** @brief 语义校验失败：ErrCode=01，不落盘、不改 RAM */
static void case_set_validate_fail(void)
{
    TEST_BEGIN("0AH：语义校验失败 → ErrCode=01，不落盘");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 1U);
    d[AH_TCP_OFF_MODULES]     = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    d[AH_TCP_OFF_MODULES + 1] = 0x01U;

    s_validate_ok       = false;
    app_ah_cfg_t before = s_cfg;
    _ah_tcp_cmd_set(&s_ccb, 0x02U, d, AH_TCP_FIXED_LEN + 2U);

    CHECK_MSG(s_tx[3] == 0x20U, "请求 SEQ=0x02 时应答 SEQ 应为 0x20");
    CHECK_MSG(s_tx[8U + AH_TCP_0A_OFF_ERR] == (uint8_t)APP_AH_ERR_PARAM, "ErrCode 应为 01");
    CHECK_MSG(s_save_calls == 0, "校验失败不应落盘");
    CHECK_MSG(memcmp(&s_cfg, &before, sizeof(before)) == 0, "校验失败不应改 RAM");
    CHECK_MSG(s_rtc_calls == 0, "校验失败不应写 RTC");
}

/** @brief 落盘失败：ErrCode=02，RAM 保持原值（不能“看着配好了”） */
static void case_set_save_fail(void)
{
    TEST_BEGIN("0AH：落盘失败 → ErrCode=02，RAM 不变");

    _reset();

    uint8_t d[AH_TCP_FIXED_LEN + 3U];
    _set_req(d, 1U);
    _put32(d + AH_TCP_OFF_UNIX_TM, 1741183200U);
    memcpy(d + AH_TCP_OFF_LANE, "\x01\x02\x03\x04\x05", 5);
    d[AH_TCP_OFF_MODULES]     = (uint8_t)BOARD_AH_DISPLAY_TYPE;
    d[AH_TCP_OFF_MODULES + 1] = 0x09U;

    s_save_rc           = -2;
    app_ah_cfg_t before = s_cfg;
    _ah_tcp_cmd_set(&s_ccb, 0x03U, d, AH_TCP_FIXED_LEN + 2U);

    CHECK_MSG(s_save_calls == 1, "应尝试落盘一次");
    CHECK_MSG(s_tx[8U + AH_TCP_0A_OFF_ERR] == (uint8_t)APP_AH_ERR_SAVE, "ErrCode 应为 02");
    CHECK_MSG(memcmp(&s_cfg, &before, sizeof(before)) == 0, "落盘失败 RAM 必须保持原值");
    CHECK_MSG(s_rtc_calls == 0, "落盘失败不应写 RTC");
}

/* ================================================================
 *  0BH 采集
 * ================================================================ */

/** @brief 0BH 应答：公共块字段与“模块数 + 本机模块”齐全 */
static void case_get_reply(void)
{
    TEST_BEGIN("0BH：应答公共块 + 本机模块");

    _reset();

    uint8_t req[AH_TCP_0B_REQ_LEN] = {0};
    req[AH_TCP_OFF_CMD]            = AH_TCP_CMD_GET;
    _put32(req + AH_TCP_OFF_UNIX_TM, 1741183200U);

    _ah_tcp_cmd_get(&s_ccb, 0x03U, req, sizeof(req));

    CHECK_MSG(s_tx_calls == 1, "应应答一帧");
    CHECK_MSG(s_tx_len == (8U + AH_TCP_FIXED_LEN + 2U + 2U), "32H 应答帧长应为 96，实际 %u",
              (unsigned)s_tx_len);
    CHECK_MSG(s_tx[3] == 0x30U, "应答 SEQ 应为 0x30，实际 0x%02X", (unsigned)s_tx[3]);
    CHECK_MSG(s_tx[8] == AH_TCP_CMD_GET, "应答 CmdType 应为 0x0B");
    CHECK_MSG(_rd32(s_tx + 8U + AH_TCP_OFF_UNIX_TM) == 1741183200U, "UnixTm 应原样回显");

    CHECK_MSG(memcmp(s_tx + 8U + AH_TCP_OFF_LANE, k_lane, 5) == 0, "LaneHex 应为当前配置");
    CHECK_MSG(memcmp(s_tx + 8U + AH_TCP_OFF_CERT, k_cert, 8) == 0, "Cert 应为当前配置");
    CHECK_MSG(memcmp(s_tx + 8U + AH_TCP_OFF_MQTT_IP, s_cfg.mqtt_ip, 4) == 0, "MQTTIP 应一致");
    CHECK_MSG(_rd16(s_tx + 8U + AH_TCP_OFF_MQTT_PORT) == 6000U, "MQTTPort 应为 6000");
    CHECK_MSG(memcmp(s_tx + 8U + AH_TCP_OFF_MQTT_USER, "pxh", 3) == 0, "MQTTUser 应为 pxh");
    CHECK_MSG(_rd16(s_tx + 8U + AH_TCP_OFF_CLIENT_PORT) == 6001U, "ClientPort 应为 6001");

    CHECK_MSG(s_tx[8U + AH_TCP_OFF_MODE_CNT] == 1U, "ModeCnt 应为 1");
    CHECK_MSG(s_tx[8U + AH_TCP_OFF_MODULES] == (uint8_t)BOARD_AH_DISPLAY_TYPE, "模块类型应为本板");
    CHECK_MSG(s_tx[8U + AH_TCP_OFF_MODULES + 1U] == 1U, "模块序号应为 1");
}

/** @brief 31H 模块应答多 1 字节 FontLine；32H 不带 */
static void case_get_module_size(void)
{
    TEST_BEGIN("0BH：31H 应答带 FontLine（+1 字节）");

    _reset();

    uint8_t req[AH_TCP_0B_REQ_LEN] = {0};
    req[AH_TCP_OFF_CMD]            = AH_TCP_CMD_GET;

    s_cfg.device_id = 0x31U;
    s_cfg.font_line = 3U;
    _ah_tcp_cmd_get(&s_ccb, 0x01U, req, sizeof(req));

    CHECK_MSG(s_tx_len == (8U + AH_TCP_FIXED_LEN + 3U + 2U), "31H 应答帧长应为 97，实际 %u",
              (unsigned)s_tx_len);
    CHECK_MSG(s_tx[8U + AH_TCP_OFF_MODULES] == 0x31U, "模块类型应为 31H");
    CHECK_MSG(s_tx[8U + AH_TCP_OFF_MODULES + 2U] == 3U, "FontLine 应为 3");

    CHECK_MSG(app_ah_tcp_module_size(0x31U) == 3U, "31H 模块长度应为 3");
    CHECK_MSG(app_ah_tcp_module_size(0x32U) == 2U, "32H 模块长度应为 2");
    CHECK_MSG(app_ah_tcp_module_size(0x12U) == 0U, "未知模块长度应为 0");
}

/** @brief 0BH 请求长度必须恰为 20：否则不应答 */
static void case_get_bad_len(void)
{
    TEST_BEGIN("0BH：请求长度不是 20 → 不应答");

    _reset();

    uint8_t req[AH_TCP_0B_REQ_LEN] = {0};
    req[AH_TCP_OFF_CMD]            = AH_TCP_CMD_GET;

    _ah_tcp_cmd_get(&s_ccb, 0x01U, req, AH_TCP_0B_REQ_LEN - 1U);
    CHECK_MSG(s_tx_calls == 0, "长度 19 不应应答，实际发了 %d 帧", s_tx_calls);

    _ah_tcp_cmd_get(&s_ccb, 0x01U, req, AH_TCP_0B_REQ_LEN + 1U);
    CHECK_MSG(s_tx_calls == 0, "长度 21 不应应答");
}

/* ================================================================ */

int main(void)
{
    case_probe_wait_head();
    case_probe_fake_head();
    case_probe_skip_unknown_version();
    case_probe_fake_crc();
    case_probe_ready();

    case_set_ok();
    case_set_zero_keeps();
    case_set_module_silent_drop();
    case_set_bad_unix_time();
    case_set_bad_format();
    case_set_validate_fail();
    case_set_save_fail();

    case_get_reply();
    case_get_module_size();
    case_get_bad_len();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
