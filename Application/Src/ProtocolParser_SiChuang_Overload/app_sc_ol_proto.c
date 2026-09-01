/**
 * @file    app_sc_ol_proto.c
 * @brief   四川治超屏协议模块（1F，3.5.1 串口方式）— 注册 / 探测 / 处理任务
 *
 * 绑定 RS485 + RS232 双通道。帧结构 `FF + 长度(≤255，1 字节) + 命令 + 亮度 + 数据 + BCC + FF`。
 * 与 RLS 协议区分：RLS 帧头 `FF FE`，长度字段 0xFE(254) 被 probe 显式排除 → 本 probe 立即 FAKE；
 * RLS probe 亦以「第二字节必须 0xFE」拒绝治超帧，双向快拒成立。
 */

#include "app_sc_ol_proto.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"

#include "app_sc_ol_proto_parse.h"
#include "app_sc_ol_proto_cmd.h"

/* 地区协议通道 RB：与青海/ETC/MTC 等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);
RB_PROVIDE_WEAK(rb_provide_rs232, RB_SIZE_RS232);

static proto_mask_t s_sc_ol_mask;
static proto_mask_t s_sc_ol_mask_rs232;
static osMessageQueueId_t s_sc_ol_queue;

/* 静态队列：payload 覆盖协议最大帧（长度字段 1 字节上限 0xFF，0xFE 排除；
 * 必须 ≥ SC_OL_FRAME_LEN_MAX，否则 250~255 长帧会被队列截断导致越界读） */
#define SC_OL_PAYLOAD_MAX (SC_OL_FRAME_LEN_MAX) /* 255：覆盖 0x80 全屏长数据段（数据 ≤249） */
#define SC_OL_MSG_SIZE    (sizeof(frame_msg_t) + SC_OL_PAYLOAD_MAX)
#define SC_OL_QUEUE_DEPTH (3U)
_Static_assert(SC_OL_PAYLOAD_MAX <= RB_SIZE_RS485, "SC_OL frame must fit RS485 RB");
_Static_assert(SC_OL_PAYLOAD_MAX <= RB_SIZE_RS232, "SC_OL frame must fit RS232 RB");
static StaticQueue_t s_sc_ol_queue_cb;
static uint8_t s_sc_ol_queue_buf[SC_OL_QUEUE_DEPTH * SC_OL_MSG_SIZE];
static const osMessageQueueAttr_t s_sc_ol_queue_attr = {
    .name    = "sc_ol_queue",
    .cb_mem  = &s_sc_ol_queue_cb,
    .cb_size = sizeof(s_sc_ol_queue_cb),
    .mq_mem  = s_sc_ol_queue_buf,
    .mq_size = sizeof(s_sc_ol_queue_buf),
};

static proto_probe_sta_t sc_ol_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                           uint32_t *total_len, uint8_t *aux);

/**
 * @brief  四川治超屏协议模块初始化入口。
 * @note   通过 initcall 自动注册：申请 RS485 / RS232 RB → 注册探测器 →
 *         绑定双通道 → 创建队列与任务。
 *
 * @warning CH_ID_RS232_1 (PL_UART6) 为语音板 TTS 专用，任何地区协议禁止绑定。
 */
void sc_ol_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_sc_ol_mask = app_proto_register(sc_ol_probe_frame, rb);
    if (s_sc_ol_mask == 0)
        return;

    app_proto_bind_channel(s_sc_ol_mask, CH_ID_RS485);

    ring_buffer_t *rb_rs232 = app_proto_acquire_buf(RB_SLOT_RS232, RB_SIZE_RS232);
    if (rb_rs232 != nullptr) {
        s_sc_ol_mask_rs232 = app_proto_register(sc_ol_probe_frame, rb_rs232);
        if (s_sc_ol_mask_rs232 != 0)
            app_proto_bind_channel(s_sc_ol_mask_rs232, CH_ID_RS232);
    }

    s_sc_ol_queue = osMessageQueueNew(SC_OL_QUEUE_DEPTH, SC_OL_MSG_SIZE, &s_sc_ol_queue_attr);
    app_proto_set_frame_queue(s_sc_ol_mask, s_sc_ol_queue);
    if (s_sc_ol_mask_rs232 != 0)
        app_proto_set_frame_queue(s_sc_ol_mask_rs232, s_sc_ol_queue);

    static const osThreadAttr_t s_sc_ol_task_attr = {
        .name       = "sc_ol_handle_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(sc_ol_proto_handle_task, NULL, &s_sc_ol_task_attr);
}
sw_app_initcall(sc_ol_proto_init);

/**
 * @brief  治超协议帧探测（PURE 函数契约：只 rb_peek，无副作用）
 *
 * 首字节快速拒绝：首字节非 0xFF → 立即 FAKE。
 * 第二字节为长度字段：合法范围 [7, 255]，RLS 的 0xFE(254) 显式排除立即 FAKE。
 * 尾字节必须 0xFF；BCC 校验在 parse 层执行。
 */
static proto_probe_sta_t sc_ol_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                           uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);

    /* 首字节快速拒绝：无数据 → FAKE；首字节非 0xFF → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(rb, 0, &first_byte, 1, NULL);
    if (first_byte != 0xFF)
        return PROTO_PROBE_FAKE;

    if (avail < 2U)
        return PROTO_PROBE_WAIT;

    uint8_t len_byte;
    rb_peek(rb, 1, &len_byte, 1, NULL);

    /* 长度下界：≥7；RLS 的 FF FE（0xFE=254）必须显式排除，否则会把 RLS 帧当治超长帧吞掉并 WAIT 卡死链式探测
     * （len_byte 为 uint8_t，上限 0xFF 由类型天然约束，无需再比） */
    if (len_byte < SC_OL_FRAME_LEN_MIN || len_byte == 0xFE)
        return PROTO_PROBE_FAKE;

    if (avail < len_byte)
        return PROTO_PROBE_WAIT;

    uint8_t tail;
    rb_peek(rb, (uint32_t)(len_byte - 1U), &tail, 1, NULL);
    if (tail != 0xFF)
        return PROTO_PROBE_FAKE;

    *total_len = len_byte;
    return PROTO_PROBE_READY;
}

/**
 * @brief  治超协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void sc_ol_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[SC_OL_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_sc_ol_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        sc_ol_parsed_cmd_t cmd = sc_ol_parse_frame(msg->data, msg->data_len);
        sc_ol_execute_cmd(msg->ch, &cmd);
    }
}