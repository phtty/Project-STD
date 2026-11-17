/**
 * @file    app_ah_tcp.c
 * @brief   AH/TCP 参数信道：帧编解码、0AH 设置、0BH 采集
 *
 * 约定：
 *   - 设备是服务端，默认端口取 `app_ah_cfg_t.client_port`（6001）；
 *   - 0AH 只做“校验 → 落盘 → 应答”，**不碰运行态**；网络/MQTT/主题都等重启；
 *   - `UnixTm` 是例外：成功落盘后立即写 RTC（它不是持久化参数）；
 *   - 模块表只接受本机类型；出现别的类型/多个模块 → **整帧静默丢弃**（不应答）。
 */

#include "app_ah_tcp.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "board.h"
#include <string.h>

#include "app_ah_cfg.h"
#include "app_tcp_server.h"
#include "crc_utils.h"
#include "initcall.h"
#include "pl_mem.h"
#include "pl_net.h"
#include "pl_rtc.h"
#include "pl_task.h"
#include "ring_buffer.h"

/* ---- 协议控制块：自有缓冲与队列 ---- */
/* 5006048 的 SRAM 余量只有个位数 KB，参数口缓冲与 MQTT/其它协议一致放 CCMRAM */
RB_DEFINE_ATTR(s_ah_tcp_rb, 2048, PL_CCMRAM);

static const app_pcb_ops_t s_ah_tcp_ops = {.probe = app_ah_tcp_probe_frame};

static app_pcb_t s_ah_tcp_pcb = {
    .name        = "ah_tcp",
    .ops         = &s_ah_tcp_ops,
    .rb          = &s_ah_tcp_rb,
    .payload_max = AH_TCP_FRAME_MAX,
};

#define AH_TCP_MSG_SIZE (sizeof(app_dispatch_msg_t) + AH_TCP_FRAME_MAX)

static osMessageQueueId_t s_ah_tcp_queue;

static StaticQueue_t s_ah_tcp_queue_cb;
static uint8_t s_ah_tcp_queue_buf[2U * AH_TCP_MSG_SIZE];
static const osMessageQueueAttr_t s_ah_tcp_queue_attr = {
    .name    = "s_ah_tcp_queue",
    .cb_mem  = &s_ah_tcp_queue_cb,
    .cb_size = sizeof(s_ah_tcp_queue_cb),
    .mq_mem  = s_ah_tcp_queue_buf,
    .mq_size = sizeof(s_ah_tcp_queue_buf),
};

static const osThreadAttr_t s_ah_tcp_task_attr = {
    .name       = "app_ah_tcp_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ================================================================
 *  大端读写与“全 0 = 保持”合并
 * ================================================================ */

static uint16_t _rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t _rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void _wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void _wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool _is_zero(const uint8_t *p, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        if (p[i] != 0U)
            return false;
    }
    return true;
}

static void _merge_bytes(uint8_t *dst, const uint8_t *src, uint16_t n)
{
    if (!_is_zero(src, n))
        memcpy(dst, src, n);
}

static uint16_t _merge_u16(uint16_t current, uint16_t v)
{
    return v != 0U ? v : current;
}

/** @brief 生成应答 SEQ：设备侧为 X0H；请求低半字节为 0 时用 0x10 */
static uint8_t _reply_seq(uint8_t req)
{
    uint8_t low = (uint8_t)(req & 0x0FU);
    return low != 0U ? (uint8_t)(low << 4) : 0x10U;
}

/* ================================================================
 *  组帧与应答
 * ================================================================ */

static void _ah_tcp_send(app_ccb_t *ccb, uint8_t seq, const uint8_t *data, uint16_t len)
{
    if (ccb == nullptr || data == nullptr || len > AH_TCP_DATA_MAX)
        return;

    uint8_t frame[AH_TCP_FRAME_MAX];
    uint16_t total = (uint16_t)(AH_TCP_FRAME_OVERHEAD + len);

    frame[0] = (uint8_t)(AH_TCP_STX >> 8);
    frame[1] = (uint8_t)AH_TCP_STX;
    frame[2] = AH_TCP_VER;
    frame[3] = _reply_seq(seq);
    frame[4] = 0U; /* LEN 高 2 字节保留 */
    frame[5] = 0U;
    frame[6] = (uint8_t)(len >> 8);
    frame[7] = (uint8_t)len;
    memcpy(frame + 8U, data, len);

    uint16_t crc    = crc16_xmodem(frame + 2U, (size_t)(6U + len));
    frame[8U + len] = (uint8_t)(crc >> 8);
    frame[9U + len] = (uint8_t)crc;

    if (app_ccb_send_to(ccb, nullptr, frame, total) < 0)
        printf("[ah/tcp] 应答发送失败（cmd=0x%02X len=%u）\n", data[0], (unsigned)len);
}

static void _ah_tcp_reply_set(app_ccb_t *ccb, uint8_t seq, const app_ah_cfg_t *cfg,
                              uint32_t unix_tm, app_ah_err_t err)
{
    if (cfg == nullptr)
        return;

    uint8_t rsp[AH_TCP_0A_RSP_LEN] = {0};
    rsp[AH_TCP_OFF_CMD]            = AH_TCP_CMD_SET;
    _wr32(rsp + AH_TCP_OFF_UNIX_TM, unix_tm);
    memcpy(rsp + AH_TCP_OFF_LANE, cfg->lane_hex, sizeof(cfg->lane_hex));
    memcpy(rsp + AH_TCP_OFF_CERT, cfg->cert, sizeof(cfg->cert));
    /* RSV 保持 0 */
    rsp[AH_TCP_0A_OFF_ERR] = (uint8_t)err;
    _ah_tcp_send(ccb, seq, rsp, (uint16_t)sizeof(rsp));
}

static void _ah_tcp_reply_get(app_ccb_t *ccb, uint8_t seq, const app_ah_cfg_t *cfg,
                              uint32_t unix_tm)
{
    if (cfg == nullptr)
        return;

    uint8_t rsp[AH_TCP_0B_RSP_MAX] = {0};

    rsp[AH_TCP_OFF_CMD] = AH_TCP_CMD_GET;
    _wr32(rsp + AH_TCP_OFF_UNIX_TM, unix_tm);
    memcpy(rsp + AH_TCP_OFF_LANE, cfg->lane_hex, sizeof(cfg->lane_hex));
    memcpy(rsp + AH_TCP_OFF_CERT, cfg->cert, sizeof(cfg->cert));

    memcpy(rsp + AH_TCP_OFF_SERVER1_IP, cfg->server1_ip, sizeof(cfg->server1_ip));
    _wr16(rsp + AH_TCP_OFF_SERVER1_PORT, cfg->server1_port);
    memcpy(rsp + AH_TCP_OFF_SERVER2_IP, cfg->server2_ip, sizeof(cfg->server2_ip));
    _wr16(rsp + AH_TCP_OFF_SERVER2_PORT, cfg->server2_port);
    memcpy(rsp + AH_TCP_OFF_SERVER3_IP, cfg->server3_ip, sizeof(cfg->server3_ip));
    _wr16(rsp + AH_TCP_OFF_SERVER3_PORT, cfg->server3_port);

    memcpy(rsp + AH_TCP_OFF_MQTT_IP, cfg->mqtt_ip, sizeof(cfg->mqtt_ip));
    _wr16(rsp + AH_TCP_OFF_MQTT_PORT, cfg->mqtt_port);
    memcpy(rsp + AH_TCP_OFF_MQTT_USER, cfg->mqtt_user, sizeof(cfg->mqtt_user));
    memcpy(rsp + AH_TCP_OFF_MQTT_PWD, cfg->mqtt_pwd, sizeof(cfg->mqtt_pwd));
    rsp[AH_TCP_OFF_MQTT_QOS] = cfg->mqtt_qos;

    memcpy(rsp + AH_TCP_OFF_NTP_IP, cfg->ntp_ip, sizeof(cfg->ntp_ip));
    memcpy(rsp + AH_TCP_OFF_CLIENT_IP, cfg->client_ip, sizeof(cfg->client_ip));
    _wr16(rsp + AH_TCP_OFF_CLIENT_PORT, cfg->client_port);
    memcpy(rsp + AH_TCP_OFF_GATEWAY, cfg->gateway, sizeof(cfg->gateway));
    memcpy(rsp + AH_TCP_OFF_NETMASK, cfg->netmask, sizeof(cfg->netmask));

    /* 本机只有一个显示模块 */
    rsp[AH_TCP_OFF_MODE_CNT] = 1U;
    uint16_t n               = AH_TCP_OFF_MODULES;
    rsp[n++]                 = cfg->device_id;
    rsp[n++]                 = cfg->device_no;
    if (cfg->device_id == (uint8_t)AH_DEV_FEE_DISPLAY)
        rsp[n++] = cfg->font_line;

    _ah_tcp_send(ccb, seq, rsp, n);
}

/* ================================================================
 *  0AH / 0BH
 * ================================================================ */

/** @brief 0AH：把 DATA 合并进 staged 配置，成功后落盘；不碰运行态 */
static void _ah_tcp_cmd_set(app_ccb_t *ccb, uint8_t seq, const uint8_t *data, uint16_t len)
{
    app_ah_cfg_t current;
    app_ah_cfg_get(&current);

    if (len < AH_TCP_FIXED_LEN) {
        _ah_tcp_reply_set(ccb, seq, &current, 0U, APP_AH_ERR_FORMAT);
        return;
    }

    uint32_t unix_tm = _rd32(data + AH_TCP_OFF_UNIX_TM);
    if (unix_tm != 0U && (unix_tm < 1577836800U || unix_tm > 4102444800U)) {
        _ah_tcp_reply_set(ccb, seq, &current, unix_tm, APP_AH_ERR_PARAM);
        return;
    }

    app_ah_cfg_t staged = current;
    _merge_bytes(staged.lane_hex, data + AH_TCP_OFF_LANE, sizeof(staged.lane_hex));
    _merge_bytes(staged.cert, data + AH_TCP_OFF_CERT, sizeof(staged.cert));

    _merge_bytes(staged.server1_ip, data + AH_TCP_OFF_SERVER1_IP, sizeof(staged.server1_ip));
    staged.server1_port = _merge_u16(staged.server1_port, _rd16(data + AH_TCP_OFF_SERVER1_PORT));
    _merge_bytes(staged.server2_ip, data + AH_TCP_OFF_SERVER2_IP, sizeof(staged.server2_ip));
    staged.server2_port = _merge_u16(staged.server2_port, _rd16(data + AH_TCP_OFF_SERVER2_PORT));
    _merge_bytes(staged.server3_ip, data + AH_TCP_OFF_SERVER3_IP, sizeof(staged.server3_ip));
    staged.server3_port = _merge_u16(staged.server3_port, _rd16(data + AH_TCP_OFF_SERVER3_PORT));

    _merge_bytes(staged.mqtt_ip, data + AH_TCP_OFF_MQTT_IP, sizeof(staged.mqtt_ip));
    staged.mqtt_port = _merge_u16(staged.mqtt_port, _rd16(data + AH_TCP_OFF_MQTT_PORT));
    _merge_bytes((uint8_t *)staged.mqtt_user, data + AH_TCP_OFF_MQTT_USER,
                 sizeof(staged.mqtt_user));
    _merge_bytes((uint8_t *)staged.mqtt_pwd, data + AH_TCP_OFF_MQTT_PWD, sizeof(staged.mqtt_pwd));
    if (data[AH_TCP_OFF_MQTT_QOS] != 0U)
        staged.mqtt_qos = data[AH_TCP_OFF_MQTT_QOS];

    _merge_bytes(staged.ntp_ip, data + AH_TCP_OFF_NTP_IP, sizeof(staged.ntp_ip));
    _merge_bytes(staged.client_ip, data + AH_TCP_OFF_CLIENT_IP, sizeof(staged.client_ip));
    staged.client_port = _merge_u16(staged.client_port, _rd16(data + AH_TCP_OFF_CLIENT_PORT));
    _merge_bytes(staged.gateway, data + AH_TCP_OFF_GATEWAY, sizeof(staged.gateway));
    _merge_bytes(staged.netmask, data + AH_TCP_OFF_NETMASK, sizeof(staged.netmask));

    /* 模块表：0 = 不改；1 且是本机类型 = 采纳；其余整帧静默丢弃 */
    uint8_t mode_cnt = data[AH_TCP_OFF_MODE_CNT];
    if (mode_cnt == 0U) {
        if (len != AH_TCP_FIXED_LEN) {
            printf("[ah/tcp] 0AH ModeCnt=0 但长度 %u 不是 84，丢弃\n", (unsigned)len);
            return;
        }
    } else if (mode_cnt == 1U) {
        if (len < AH_TCP_FIXED_LEN + 1U) {
            printf("[ah/tcp] 0AH ModeCnt=1 但缺少模块头，丢弃\n");
            return;
        }
        uint8_t dev = data[AH_TCP_OFF_MODULES];
        if (dev != (uint8_t)BOARD_AH_DISPLAY_TYPE) {
            printf("[ah/tcp] 0AH 模块类型 0x%02X 不是本机 0x%02X，整帧丢弃\n", (unsigned)dev,
                   (unsigned)BOARD_AH_DISPLAY_TYPE);
            return;
        }
        uint8_t size = app_ah_tcp_module_size(dev);
        if (size == 0U || len != (uint16_t)(AH_TCP_FIXED_LEN + size)) {
            printf("[ah/tcp] 0AH 模块长度 %u 与类型 0x%02X 不符，丢弃\n", (unsigned)len,
                   (unsigned)dev);
            return;
        }
        uint8_t no = data[AH_TCP_OFF_MODULES + 1U];
        if (no != 0U)
            staged.device_no = no;
        if (dev == (uint8_t)AH_DEV_FEE_DISPLAY) {
            uint8_t font_line = data[AH_TCP_OFF_MODULES + 2U];
            if (font_line > APP_AH_FONT_LINE_MAX) {
                _ah_tcp_reply_set(ccb, seq, &current, unix_tm, APP_AH_ERR_PARAM);
                return;
            }
            staged.font_line = font_line;
        } else {
            staged.font_line = 0U;
        }
        staged.device_id = dev;
    } else {
        printf("[ah/tcp] 0AH ModeCnt=%u 超出本机能力，整帧丢弃\n", (unsigned)mode_cnt);
        return;
    }

    if (!app_ah_cfg_validate(&staged, nullptr)) {
        _ah_tcp_reply_set(ccb, seq, &current, unix_tm, APP_AH_ERR_PARAM);
        return;
    }
    if (app_ah_cfg_save(&staged) != 0) {
        _ah_tcp_reply_set(ccb, seq, &current, unix_tm, APP_AH_ERR_SAVE);
        return;
    }

    /* UnixTm 不是持久化参数：落盘成功后立即校时 */
    if (unix_tm != 0U && !pl_rtc_set_timestamp(pl_rtc_get_handle(), unix_tm))
        printf("[ah/tcp] RTC 写入失败（%u）\n", (unsigned)unix_tm);

    app_ah_cfg_t committed;
    app_ah_cfg_get(&committed);
    _ah_tcp_reply_set(ccb, seq, &committed, unix_tm, APP_AH_ERR_OK);
}

/** @brief 0BH：返回本机当前配置；长度不对则不应答（无 ErrCode 字段） */
static void _ah_tcp_cmd_get(app_ccb_t *ccb, uint8_t seq, const uint8_t *data, uint16_t len)
{
    if (len != AH_TCP_0B_REQ_LEN) {
        printf("[ah/tcp] 0BH 请求长度 %u 不是 20，丢弃\n", (unsigned)len);
        return;
    }

    uint32_t unix_tm = _rd32(data + AH_TCP_OFF_UNIX_TM);
    app_ah_cfg_t cfg;
    app_ah_cfg_get(&cfg);
    _ah_tcp_reply_get(ccb, seq, &cfg, unix_tm);
}

/* ================================================================
 *  探针 / 任务
 * ================================================================ */

app_pcb_probe_state_t app_ah_tcp_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                             const app_ccb_src_t *src, uint8_t *scratch,
                                             uint16_t scratch_size, uint32_t *total_len,
                                             uint8_t *aux)
{
    (void)ccb;
    (void)src;

    uint16_t n = rb_peek(self->rb, 0, scratch, scratch_size, nullptr);
    if (n < 8U)
        return APP_PCB_PROBE_STATE_WAIT;

    if (scratch[0] != 0xFFU || scratch[1] != 0xFFU)
        return APP_PCB_PROBE_STATE_FAKE;

    uint16_t len = _rd16(scratch + 6);
    if (len == 0U || len > AH_TCP_DATA_MAX)
        return APP_PCB_PROBE_STATE_FAKE;

    uint32_t total = AH_TCP_FRAME_OVERHEAD + (uint32_t)len;
    if (n < total)
        return APP_PCB_PROBE_STATE_WAIT;

    /* 版本/保留字节不认识：帧长仍可信，整帧跳过，别逐字节重扫 */
    if (scratch[2] != AH_TCP_VER || scratch[4] != 0U || scratch[5] != 0U) {
        *total_len = total;
        return APP_PCB_PROBE_STATE_SKIP;
    }

    uint16_t crc = _rd16(scratch + 8U + len);
    if (crc16_xmodem(scratch + 2U, (size_t)(6U + len)) != crc)
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = total;
    *aux       = scratch[8];
    return APP_PCB_PROBE_STATE_READY;
}

void app_ah_tcp_task(void *argument)
{
    (void)argument;

    static uint8_t msg_buf[AH_TCP_MSG_SIZE];
    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)msg_buf;

    for (;;) {
        if (osMessageQueueGet(s_ah_tcp_queue, msg, nullptr, osWaitForever) != osOK)
            continue;
        if (msg->data_len < (AH_TCP_FRAME_OVERHEAD + 1U))
            continue;

        const uint8_t *frame = msg->data;
        uint16_t len         = (uint16_t)(msg->data_len - AH_TCP_FRAME_OVERHEAD);
        const uint8_t *data  = frame + 8U;
        uint8_t seq          = frame[3];

        switch (msg->aux) {
            case AH_TCP_CMD_SET:
                _ah_tcp_cmd_set(msg->ccb, seq, data, len);
                break;
            case AH_TCP_CMD_GET:
                _ah_tcp_cmd_get(msg->ccb, seq, data, len);
                break;
            default:
                printf("[ah/tcp] 未支持的命令 0x%02X（丢帧）\n", msg->aux);
                break;
        }
    }
}

/* ================================================================
 *  上电应用与自注册
 * ================================================================ */

static bool _ip_is_zero(const uint8_t ip[4])
{
    return ip[0] == 0U && ip[1] == 0U && ip[2] == 0U && ip[3] == 0U;
}

/** @brief 上电应用网络参数（只在记录有效且与运行态不同时调 pl_net_set_ip） */
static void _ah_tcp_apply_net(const app_ah_cfg_t *cfg)
{
    if (_ip_is_zero(cfg->client_ip) || _ip_is_zero(cfg->netmask)) {
        printf("[ah/tcp] 网络参数未配置完整，跳过上电应用\n");
        return;
    }
    if (_ip_is_zero(cfg->gateway)) {
        printf("[ah/tcp] 网关未配置，跳过上电应用\n");
        return;
    }

    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    pl_net_get_ip(ip, mask, gw);
    if (memcmp(ip, cfg->client_ip, 4) == 0 && memcmp(mask, cfg->netmask, 4) == 0 &&
        memcmp(gw, cfg->gateway, 4) == 0)
        return;

    pl_net_set_ip(cfg->client_ip, cfg->netmask, cfg->gateway);
    printf("[ah/tcp] 上电应用网络：%u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n", cfg->client_ip[0],
           cfg->client_ip[1], cfg->client_ip[2], cfg->client_ip[3], cfg->netmask[0],
           cfg->netmask[1], cfg->netmask[2], cfg->netmask[3], cfg->gateway[0], cfg->gateway[1],
           cfg->gateway[2], cfg->gateway[3]);
}

/** @brief sw_post(4)：应用网络、开参数口、绑定协议、建队列与任务 */
static void _ah_tcp_init(void)
{
    app_ah_cfg_t cfg;
    app_ah_cfg_get(&cfg);
    _ah_tcp_apply_net(&cfg);

    rb_init(&s_ah_tcp_rb, "ah_tcp");

    s_ah_tcp_queue = osMessageQueueNew(2, AH_TCP_MSG_SIZE, &s_ah_tcp_queue_attr);
    if (s_ah_tcp_queue == nullptr) {
        printf("[ah/tcp] 帧队列创建失败，参数口不启动\n");
        return;
    }
    s_ah_tcp_pcb.queue = s_ah_tcp_queue;

    uint16_t port         = cfg.client_port != 0U ? cfg.client_port : 6001U;
    app_tcp_server_t *srv = app_tcp_server_open("ah_param", port, 30000U);
    if (srv == nullptr) {
        printf("[ah/tcp] 参数口实例创建失败（端口 %u），参数口不启动\n", (unsigned)port);
        return;
    }

    app_dispatch_bind(&s_ah_tcp_pcb, app_tcp_server_ccb_of(srv));

    if (pl_task_new(app_ah_tcp_task, nullptr, &s_ah_tcp_task_attr) == nullptr) {
        printf("[ah/tcp] 协议任务创建失败，参数口不启动\n");
        return;
    }

    if (app_tcp_server_start_instance(srv) == nullptr) {
        printf("[ah/tcp] 监听任务创建失败（端口 %u），参数口不启动\n", (unsigned)port);
        return;
    }
    printf("[ah/tcp] 参数口已启动：端口 %u\n", (unsigned)port);
}
sw_post_initcall(_ah_tcp_init);
