/**
 * @file    app_lcsp.c
 * @brief   LCSP 帧探测、协议任务与模块初始化
 *
 * 任务模型与 CDSTD/LDI 一致：探针在分发任务里定界（只窥视），整帧投进本协议队列，
 * 本协议任务取帧后分派到命令表。区别只在**应答**：本协议的每条请求都可能要回帧，
 * 所以"发"与"判重"都收在本文件（见 `app_lcsp_task`），命令层只负责组包。
 */

#include "app_lcsp.h"

#include <string.h>

#include "FreeRTOS.h" /* StaticQueue_t（静态队列控制块；host 测试由 test/stubs 提供）*/
#include "cmsis_os2.h"
#include "initcall.h"

#include "crc_utils.h"

#include "pl_mem.h"
#include "pl_net.h"
#include "pl_sys.h"
#include "pl_task.h"

#include "app_light_sensor.h"
#include "app_screen.h"
#include "app_tcp_server.h"
#include "app_udp.h"

#include "app_lcsp_cfg.h"
#include "app_lcsp_cmd.h"

/* ---- 协议私有资源（静态分配，无堆）---- */

#define LCSP_MSG_SIZE (sizeof(app_dispatch_msg_t) + LCSP_FRAME_MAX_LEN)

static StaticQueue_t s_lcsp_queue_cb;
static uint8_t s_lcsp_queue_buf[2 * LCSP_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_lcsp_queue_attr = {
    .name    = "proto_lcsp_queue",
    .cb_mem  = &s_lcsp_queue_cb,
    .cb_size = sizeof(s_lcsp_queue_cb),
    .mq_mem  = s_lcsp_queue_buf,
    .mq_size = sizeof(s_lcsp_queue_buf),
};

static osMessageQueueId_t s_lcsp_msg_queue;
static osThreadId_t s_lcsp_task_handle;
static const osThreadAttr_t s_lcsp_task_attr = {
    .name       = "app_lcsp_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

void app_lcsp_task(void *argument);

/* 收包缓冲：至少容得下"一整帧 + 半帧"（TCP 分段的尾巴与下一帧的开头可能在同一批里） */
RB_DEFINE_ATTR(s_lcsp_rb, 2 * LCSP_FRAME_MAX_LEN, PL_CCMRAM);

static const app_pcb_ops_t s_lcsp_ops = {.probe = app_lcsp_probe_frame};
static app_pcb_t s_lcsp_pcb           = {
    .name        = "lcsp",
    .ops         = &s_lcsp_ops,
    .rb          = &s_lcsp_rb,
    .payload_max = LCSP_FRAME_MAX_LEN,
};

/* ---- 重复帧判重（协议 4.3.4）----
 *
 * 上位机没收到应答会用**同一个 SEQ** 重发，帧内容逐字节一致。设备这边要"不重复执行
 * 业务动作、但仍回同样的应答"：
 *   · 不判重 → 一幅图重存一次槽位 = 一次 64KB 扇区擦写；重启命令被重发会重启两次；
 *   · 只丢不答 → 上位机一直收不到应答，按 N1 判链路故障、断链重连（现场表现：
 *     链路"时好时坏"，其实每次都是应答丢在设备这边）。
 * 缓存的是**上次发出的应答原样**，所以判重命中时不重新组包（组包依赖当时的屏状态，
 * 重算可能算出不同的内容 —— 那就不是"同样的应答"了）。
 *
 * 判重键是 (通道, SEQ, CMD)：只有 SEQ 不够 —— 上位机换了一条连接、或自己复位后
 * 从 0 重新计数时，同一序号是完全不同的命令。 */
static uint8_t s_last_reply[LCSP_REPLY_MAX_LEN];
static uint16_t s_last_reply_len;
static uint8_t s_last_reply_seq[2];
static uint8_t s_last_reply_cmd;
static bool s_last_reply_broadcast;
static bool s_last_reply_valid;
static const app_ccb_t *s_last_reply_ccb;

/* ================================================================
 *  组帧
 * ================================================================ */

uint16_t app_lcsp_build_frame(uint8_t cmd, const uint8_t seq[2], const uint8_t *data, uint16_t data_len,
                              uint8_t *out, uint16_t out_cap)
{
    if ((nullptr == out) || (nullptr == seq) || (data_len > LCSP_DATA_MAX_LEN))
        return 0;

    uint16_t total = (uint16_t)(9U + data_len);
    if (total > out_cap)
        return 0;

    out[0] = LCSP_SOF_FIRST_BYTE;
    out[1] = LCSP_SOF_SECOND_BYTE;
    out[2] = seq[0];
    out[3] = seq[1];
    out[4] = cmd;
    out[5] = (uint8_t)(data_len >> 8);
    out[6] = (uint8_t)(data_len & 0xFFU);
    if (data_len != 0U)
        memcpy(&out[7], data, data_len);

    /* 覆盖 SEQ 起至 DATA 末（协议 5.1）：从 out[2] 起，共 5 + data_len 字节 */
    uint16_t crc      = crc16_xmodem(&out[2], (size_t)(5U + data_len));
    out[7 + data_len] = (uint8_t)(crc & 0xFFU); /* 低字节在前 */
    out[8 + data_len] = (uint8_t)(crc >> 8);

    return total;
}

/* ================================================================
 *  探针
 * ================================================================ */

app_pcb_probe_state_t app_lcsp_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                           uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                           uint8_t *aux)
{
    (void)ccb;
    (void)src;

    const ring_buffer_t *buff = self->rb;
    uint32_t available        = rb_avail(buff, nullptr);

    /* 最小帧 = 9 字节（LEN=0）。逐字节比对帧头而不是读 uint16：本字段是**线格式**，
       用主机字节序去比较等于把端序约定藏在编译器里。 */
    if (available < 9U)
        return APP_PCB_PROBE_STATE_WAIT;

    uint8_t sof[2] = {0};
    rb_peek(buff, 0, sof, sizeof(sof), nullptr);
    if ((LCSP_SOF_FIRST_BYTE != sof[0]) || (LCSP_SOF_SECOND_BYTE != sof[1]))
        return APP_PCB_PROBE_STATE_FAKE;

    uint8_t len_bytes[2] = {0};
    rb_peek(buff, 5, len_bytes, sizeof(len_bytes), nullptr);
    uint16_t payload_len = (uint16_t)(((uint16_t)len_bytes[0] << 8) | len_bytes[1]);

    /* 协议 3.3：LEN 超过 8192 视为失步（噪声里凑巧命中 55 AA 的情形），
       按伪帧头逐字节滑动重找 —— 不能 WAIT（那会等一个永远不会来的长度）。 */
    if (payload_len > LCSP_DATA_MAX_LEN)
        return APP_PCB_PROBE_STATE_FAKE;

    uint32_t full_len = 9U + payload_len;

    /* 整帧到齐闸门**必须在 CRC 之前**：TCP 分段时先到的半帧如果先比 CRC，
       每一段都会被判坏帧 —— 现场表现是"大帧（显示图片）永远不生效，小帧正常"。 */
    if (available < full_len)
        return APP_PCB_PROBE_STATE_WAIT;

    /* 帧结构合法但超出本工程承载上限（更大屏体的图片）：整帧跳过。
       这里不能返回 FAKE —— 那会让框架只前进 1 字节，把同一批里的后续真帧一起啃掉。 */
    if (full_len > scratch_size) {
        *total_len = full_len;
        return APP_PCB_PROBE_STATE_SKIP;
    }

    rb_peek(buff, 0, scratch, scratch_size, nullptr);
    const app_lcsp_frame_t *frame = (const app_lcsp_frame_t *)scratch;

    uint16_t crc_calc  = crc16_xmodem(scratch + 2, (size_t)(5U + payload_len));
    uint16_t crc_frame = (uint16_t)((uint16_t)frame->data[payload_len] | ((uint16_t)frame->data[payload_len + 1] << 8));
    if (crc_calc != crc_frame)
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = full_len;
    *aux       = frame->cmd;
    return APP_PCB_PROBE_STATE_READY;
}

/* ================================================================
 *  发送
 * ================================================================ */

/** @brief 发一帧应答；`broadcast` 只在 UDP 通道上有效（TCP 通道忽略目的地）
 *  @param ccb       应答通道
 *  @param broadcast true = UDP 广播（协议 7.3.2 的 IP 应答）
 *  @param buf       整帧
 *  @param len       字节数 */
static void _lcsp_send_reply(app_ccb_t *ccb, bool broadcast, const uint8_t *buf, uint16_t len)
{
    const app_ccb_dst_t dst = {.broadcast = broadcast};

    if (nullptr == ccb)
        return;

    (void)app_ccb_send_to(ccb, &dst, buf, len);
}

/* ================================================================
 *  上电应用配置
 * ================================================================ */

/** @brief 应用亮度与屏开关记录
 *
 *  与 `0x07`/`0x08` 的处理走**同一组 API**（`app_light_sensor_set_fixed` /
 *  `app_screen_oe_*`），否则上电与运行期会各有一套口径，表现为"上电后亮度对，
 *  发一次命令就变了"这类难查的漂移。 */
static void _lcsp_apply_display_cfg(const app_lcsp_cfg_t *cfg)
{
    if (LCSP_BRIGHTNESS_AUTO == cfg->brightness)
        app_light_sensor_resume();
    else
        app_light_sensor_set_fixed(app_lcsp_level_from_percent(cfg->brightness));

    if (0U == cfg->oe_enabled)
        app_screen_oe_disable();
}

/** @brief 上电应用监听端口（以及记录里的 IP/亮度/开关）
 *
 *  **无记录时只落端口、不动 IP**：端口是"本协议怎么被找到"，出厂就该是协议规定的
 *  10028；IP 则相反 —— 上位机可能已经按板上默认地址连上了，此时把地址改成协议
 *  出厂值（192.168.1.100）等于当场失联，而现场没有任何手段再连回来。
 *
 *  端口同时施加到 TCP 与 UDP：协议 3.1 要求两者都监听 10028。**这不是端口竞争** ——
 *  LCSP 与 LDI/CDSTD/RLS 不同时启用（产品口径，同一台设备只接一种上位机协议），
 *  设备监听哪个端口由本模块一家定义。若哪天真把两者编进同一份固件，后跑的 sw_post
 *  会覆盖前者，那时该查的是产品配置。 */
static void _lcsp_apply_cfg(void)
{
    app_lcsp_cfg_t cfg;

    if (!app_lcsp_cfg_load(&cfg)) {
        app_tcp_server_set_port(LCSP_PORT_DEFAULT);
        app_udp_set_port(LCSP_PORT_DEFAULT);
        return;
    }

    pl_net_set_ip(cfg.ip, cfg.mask, cfg.gw);

    uint16_t port = (uint16_t)(((uint16_t)cfg.port[0] << 8) | cfg.port[1]);
    if (0U != port) {
        app_tcp_server_set_port(port);
        app_udp_set_port(port);
    }

    _lcsp_apply_display_cfg(&cfg);
}

/* ================================================================
 *  任务与初始化
 * ================================================================ */

void app_lcsp_task(void *argument)
{
    (void)argument;

    /* 两个 static 都放在任务栈外：一帧最大 2.8KB，384 字节栈放不下；
       任务单实例，故不必加锁。 */
    static uint8_t s_msg_buf[LCSP_MSG_SIZE];
    static uint8_t s_reply_buf[LCSP_REPLY_MAX_LEN];

    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)s_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_lcsp_msg_queue, msg, NULL, 100))
            continue;

        app_lcsp_frame_t *frame = (app_lcsp_frame_t *)msg->data;
        uint16_t payload_len    = (uint16_t)(((uint16_t)frame->len[0] << 8) | frame->len[1]);

        /* 判重命中：原样重发上次那份应答，业务动作一个都不做（见文件头的说明） */
        if (s_last_reply_valid && (msg->ccb == s_last_reply_ccb) && (0 == memcmp(s_last_reply_seq, frame->seq, sizeof(s_last_reply_seq))) && (frame->cmd == s_last_reply_cmd)) {
            _lcsp_send_reply((app_ccb_t *)s_last_reply_ccb, s_last_reply_broadcast, s_last_reply,
                             s_last_reply_len);
            continue;
        }

        app_lcsp_req_t req = {
            .ccb         = msg->ccb,
            .frame       = frame,
            .payload     = frame->data,
            .payload_len = payload_len,
            .reply_buf   = s_reply_buf,
            .reply_cap   = (uint16_t)sizeof(s_reply_buf),
        };

        uint8_t function = (uint8_t)(frame->cmd & 0x0FU);
        if ((frame->cmd != function) || (function >= APP_LCSP_CMD_COUNT) || (nullptr == g_lcsp_cmd_table[function])) {
            /* 未定义请求码：回通用错误 0xF0 并回显原命令字（协议 4.2 约束 4）。
               回显是现场唯一能定位"是哪一个码没实现"的信息。 */
            uint8_t rdata[2] = {(uint8_t)APP_LCSP_STATUS_FAIL, frame->cmd};
            req.reply_len    = app_lcsp_build_frame(APP_LCSP_CMD_ERROR_REPLY, frame->seq, rdata, sizeof(rdata),
                                                    req.reply_buf, req.reply_cap);
        } else {
            g_lcsp_cmd_table[function](&req);
        }

        if (0U == req.reply_len)
            continue;

        _lcsp_send_reply(req.ccb, req.reply_broadcast, req.reply_buf, req.reply_len);

        memcpy(s_last_reply, req.reply_buf, req.reply_len);
        memcpy(s_last_reply_seq, frame->seq, sizeof(s_last_reply_seq));
        s_last_reply_cmd       = frame->cmd;
        s_last_reply_broadcast = req.reply_broadcast;
        s_last_reply_len       = req.reply_len;
        s_last_reply_ccb       = req.ccb;
        s_last_reply_valid     = true;

        if (req.reboot_after_reply) {
            /* 协议 7.9.1：先应答、后复位，中间 ≥200ms —— 应答还没从 LwIP 发出去就复位，
               上位机会一直等 0x90 然后按"链路故障"处理。 */
            osDelay(LCSP_REBOOT_DELAY_MS);
            pl_system_reset();
        }
    }
}

[[maybe_unused]] static void _lcsp_module_init(void)
{
    _lcsp_apply_cfg();

    rb_init(&s_lcsp_rb, "lcsp");

    s_lcsp_msg_queue = osMessageQueueNew(2, LCSP_MSG_SIZE, &s_lcsp_queue_attr);
    s_lcsp_pcb.queue = s_lcsp_msg_queue;

    /* 协议 3.1/3.2：业务走 TCP，发现与 IP 查询走 UDP；两者同一端口 */
    app_dispatch_bind(&s_lcsp_pcb, app_tcp_server_ccb());
    app_dispatch_bind(&s_lcsp_pcb, app_udp_ccb());

    s_lcsp_task_handle = pl_task_new(app_lcsp_task, nullptr, &s_lcsp_task_attr);
}
sw_post_initcall(_lcsp_module_init);
