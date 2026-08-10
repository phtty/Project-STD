#include "app_qh_proto.h"

#include "initcall.h"
#include "app_dispatch.h"
#include "app_rs232.h"

#include "app_qh_proto_parse.h"
#include "app_qh_proto_cmd.h"

static proto_mask_t s_qh_mask;
static osMessageQueueId_t s_qh_queue;

static const osMessageQueueAttr_t s_qh_queue_attr = {
    .name = "qh_queue",
};

static proto_probe_sta_t qh_proto_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                              uint32_t *total_len, uint8_t *aux);

/**
 * @brief  青海协议模块初始化入口。
 * @note   通过 initcall 自动注册到系统启动流程，负责：
 *         1. 申请协议解析环形缓冲区
 *         2. 注册协议探测器到多协议分发器
 *         3. 绑定 RS232 通道
 *         4. 创建协议消息队列与处理任务
 */
void qh_proto_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(1, 512);
    s_qh_mask         = app_proto_register(qh_proto_probe_frame, rb);
    if (s_qh_mask == 0) return;
    app_proto_bind_channel(s_qh_mask, CH_ID_RS485);
    s_qh_queue = osMessageQueueNew(2, sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN, &s_qh_queue_attr);
    app_proto_set_frame_queue(s_qh_mask, s_qh_queue);

    static const osThreadAttr_t s_qh_task_attr = {
        .name       = "qh_handle_task",
        .stack_size = 512 * 4,
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
static proto_probe_sta_t qh_proto_probe_frame(const channel_t *ch, const ring_buffer_t *rb,
                                              uint32_t *total_len, uint8_t *aux)
{
    (void)ch;
    (void)aux;
    uint32_t avail = rb_avail(rb, NULL);
    if (avail < 4U) return PROTO_PROBE_WAIT;

    uint8_t head[3];
    rb_peek(rb, 0, head, sizeof(head), NULL);
    if (head[0] != '{') return PROTO_PROBE_FAKE;
    if ((head[1] < '1' || head[1] > '9') && head[1] != 'A' && head[1] != 'B') return PROTO_PROBE_FAKE;

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t data_len = head[2];
    *total_len        = (uint32_t)data_len + 4U;
    if (avail < *total_len) return PROTO_PROBE_WAIT;
    uint8_t tail;
    rb_peek(rb, *total_len - 1U, &tail, 1, NULL);
    if (tail != '}') return PROTO_PROBE_FAKE;
    return PROTO_PROBE_READY;
}

/**
 * @brief  青海协议帧处理任务。
 * @param  argument  任务参数，当前未使用。
 */
void qh_proto_handle_task(void *argument)
{
    (void)argument;
    static uint8_t msg_buf[sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN];
    frame_msg_t *msg = (frame_msg_t *)msg_buf;
    for (;;) {
        if (osOK != osMessageQueueGet(s_qh_queue, msg, NULL, osWaitForever)) {
            continue;
        }
        qh_parsed_cmd_t cmd = qh_parse_frame(msg->data, msg->data_len);
        qh_execute_cmd(msg->ch, &cmd);
    }
}
