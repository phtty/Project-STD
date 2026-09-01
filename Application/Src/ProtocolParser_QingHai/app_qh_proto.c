#include "app_qh_proto.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "app_dispatch.h"

#include "app_qh_proto_parse.h"
#include "app_qh_proto_cmd.h"

/* '{' 帧族互斥守卫：同一构建只允许编入一个 '{' 帧族协议（青海/山东/贵州/四川MTC）。
 * 多个同时编入 → 链接期 multiple definition 强制报错；
 * Makefile 全协议开发构建定义 STD_ALL_PROTO 跳过守卫（共存由 probe 注册序与文档纪律约束）。 */
#ifndef STD_ALL_PROTO
__attribute__((used)) char g_brace_proto_guard;
#endif

/* 地区协议通道 RB：与 RLS（485）等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);
RB_PROVIDE_WEAK(rb_provide_rs232, RB_SIZE_RS232);

static proto_mask_t s_qh_mask;
static proto_mask_t s_qh_mask_rs232;
static osMessageQueueId_t s_qh_queue;

/* 静态队列：payload 对齐协议上限（len 字段 1B → 帧 ≤259），勿用 IAP 的 1044 */
#define QH_PAYLOAD_MAX  (259U) /* '{' + cmd + len + data[0..255] + '}' */
#define QH_MSG_SIZE     (sizeof(frame_msg_t) + QH_PAYLOAD_MAX)
#define QH_QUEUE_DEPTH  (3U)
_Static_assert(QH_PAYLOAD_MAX <= RB_SIZE_RS485, "QH frame must fit RS485 RB");
_Static_assert(QH_PAYLOAD_MAX <= RB_SIZE_RS232, "QH frame must fit RS232 RB");
static StaticQueue_t s_qh_queue_cb;
static uint8_t s_qh_queue_buf[QH_QUEUE_DEPTH * QH_MSG_SIZE];
static const osMessageQueueAttr_t s_qh_queue_attr = {
    .name    = "qh_queue",
    .cb_mem  = &s_qh_queue_cb,
    .cb_size = sizeof(s_qh_queue_cb),
    .mq_mem  = s_qh_queue_buf,
    .mq_size = sizeof(s_qh_queue_buf),
};

static proto_probe_sta_t qh_proto_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                              uint32_t *total_len, uint8_t *aux);

/**
 * @brief  青海协议模块初始化入口。
 * @note   通过 initcall 自动注册到系统启动流程，负责：
 *         1. 申请 RS485 / RS232 物理通道 RB
 *         2. 注册协议探测器到多协议分发器
 *         3. 绑定 RS485 + RS232（RS232_1 禁止绑定 — 语音专用）
 *         4. 创建协议消息队列与处理任务
 *
 * @warning CH_ID_RS232_1 (PL_UART6) 为语音板 TTS 专用，任何地区协议禁止绑定。
 */
void qh_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_qh_mask = app_proto_register(qh_proto_probe_frame, rb);
    if (s_qh_mask == 0)
        return;

    app_proto_bind_channel(s_qh_mask, CH_ID_RS485);

    ring_buffer_t *rb_rs232 = app_proto_acquire_buf(RB_SLOT_RS232, RB_SIZE_RS232);
    if (rb_rs232 != nullptr) {
        s_qh_mask_rs232 = app_proto_register(qh_proto_probe_frame, rb_rs232);
        if (s_qh_mask_rs232 != 0)
            app_proto_bind_channel(s_qh_mask_rs232, CH_ID_RS232);
    }

    s_qh_queue = osMessageQueueNew(QH_QUEUE_DEPTH, QH_MSG_SIZE, &s_qh_queue_attr);
    app_proto_set_frame_queue(s_qh_mask, s_qh_queue);
    if (s_qh_mask_rs232 != 0)
        app_proto_set_frame_queue(s_qh_mask_rs232, s_qh_queue);

    static const osThreadAttr_t s_qh_task_attr = {
        .name       = "qh_handle_task",
        .stack_size = 256 * 4, /* 帧缓冲为 static；原 2KB 偏大 */
        .priority   = osPriorityNormal,
    };
    osThreadNew(qh_proto_handle_task, NULL, &s_qh_task_attr);
}
sw_app_initcall(qh_proto_init);

/**
 * @brief  青海协议帧探测函数。
 * @param  ch         当前接收通道。
 * @param  rb         通道环形缓冲区。
 * @param  total_len  探测到的完整帧长度。
 * @param  aux        额外状态位，当前未使用。
 * @return PROTO_PROBE_WAIT / PROTO_PROBE_FAKE / PROTO_PROBE_READY
 * @note   帧格式为 `{` + 命令 + 二进制长度 + 数据 + `}`。
 */
/**
 * @brief 青海协议帧探测（PURE 函数契约：只 peek，无副作用）
 *
 * 首字节快速拒绝：首字节非 '{' (0x7B) → 立即 FAKE，禁止无数据时盲 WAIT。
 */
static proto_probe_sta_t qh_proto_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                              uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);

    /* 首字节快速拒绝：无数据 → FAKE；首字节非 '{' → FAKE */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(rb, 0, &first_byte, 1, NULL);
    if (first_byte != '{')
        return PROTO_PROBE_FAKE;

    /* 需要至少 3 字节：'{' + 命令码 + 长度字段 */
    if (avail < 3U)
        return PROTO_PROBE_WAIT;

    uint8_t head[3];
    rb_peek(rb, 0, head, sizeof(head), NULL);
    if ((head[1] < '1' || head[1] > '9') && head[1] != 'A' && head[1] != 'B')
        return PROTO_PROBE_FAKE;

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t data_len  = head[2];
    uint32_t frame_len = (uint32_t)data_len + 4U;
    if (avail < frame_len)
        return PROTO_PROBE_WAIT;

    uint8_t tail;
    rb_peek(rb, frame_len - 1U, &tail, 1, NULL);
    if (tail != '}')
        return PROTO_PROBE_FAKE;

    *total_len = frame_len; /* 仅 READY 路径写输出参数 */
    return PROTO_PROBE_READY;
}

/**
 * @brief  青海协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void qh_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[QH_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_qh_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        qh_parsed_cmd_t cmd = qh_parse_frame(msg->data, msg->data_len);
        qh_execute_cmd(msg->ch, &cmd);
    }
}
