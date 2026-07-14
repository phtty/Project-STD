#include "app_rls.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "bcc_utils.h"
#include "app_rls_cmd.h"

/* ---- proto_rls_queue 静态分配 ---- */
#define RLS_PAYLOAD_MAX (530U) /* 帧头(6B) + bitmap(512B) + BCC(1B) + 尾(2B) + 余量 */
#define RLS_MSG_SIZE (sizeof(frame_msg_t) + RLS_PAYLOAD_MAX)

static StaticQueue_t s_rls_queue_cb;
static uint8_t s_rls_queue_buf[2 * RLS_MSG_SIZE];
static const osMessageQueueAttr_t s_rls_queue_attr = {
    .name    = "proto_rls_queue",
    .cb_mem  = &s_rls_queue_cb,
    .cb_size = sizeof(s_rls_queue_cb),
    .mq_mem  = s_rls_queue_buf,
    .mq_size = sizeof(s_rls_queue_buf),
};

const static rls_cmd_type_t cmd_index_table[] = {
    RLS_CMD_TEST,
    RLS_CMD_DISPLAY,
    RLS_CMD_DISPLAY_SAVE,
};

static proto_mask_t s_rls_mask;

osMessageQueueId_t g_rls_msg_queue;
osThreadId_t g_rls_task_handle;
const osThreadAttr_t rls_task_attr = {
    .name       = "rls_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/*--- 帧任务---*/
void rls_handle_task(void *argument)
{
    static uint8_t _msg_buf[RLS_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;
    g_rls_msg_queue = osMessageQueueNew(2, RLS_MSG_SIZE, &s_rls_queue_attr);
    app_proto_set_frame_queue(s_rls_mask, g_rls_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_rls_msg_queue, msg, NULL, osWaitForever))
            continue;

        rls_frame_t *rls_frame = (rls_frame_t *)msg->data;

        /* 查表分派 */
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]); i++)
            if (cmd_index_table[i] == *(rls_cmd_type_t *)rls_frame->cmd)
                idx = i;

        if (idx < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]))
            g_rls_cmd_table[idx](msg->ch, rls_frame->data_bcc_tail);
    }
}

/* ---- 帧探测 ---- */
const static uint8_t rls_head[2] = {0xFF, 0xFE};
const static uint8_t rls_tail[2] = {0x0D, 0x0C};
proto_probe_sta_t rls_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *aux)
{
    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < sizeof(rls_frame_t) + 4)
        return PROTO_PROBE_WAIT;

    static uint8_t mem_pool[525] = {0};
    memset(mem_pool, 0, sizeof(mem_pool));
    rb_peek(buff, 0, mem_pool, avail, nullptr);
    rls_frame_t *frame = (rls_frame_t *)mem_pool;

    if (memcmp(rls_head, frame->head, sizeof(rls_head)))
        return PROTO_PROBE_FAKE;

    uint16_t data_len = (frame->length[1] & 0xFF) | ((frame->length[0] << 8) & 0xFF00);

    if (memcmp(rls_tail, frame->data_bcc_tail + sizeof(rls_frame_t), sizeof(rls_tail)))
        return PROTO_PROBE_FAKE;

    uint8_t frame_bcc = frame->data_bcc_tail[sizeof(rls_frame_t) - 1];
    uint8_t calc_bcc  = bcc_calcu(frame->data_bcc_tail, data_len - sizeof(rls_frame_t) - 4);
    if (frame_bcc != calc_bcc)
        return PROTO_PROBE_FAKE;

    (void)*aux;
    *total_len = data_len;
    return PROTO_PROBE_READY;
}

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void rls_module_init(void)
{
    // 指定协议使用的环形缓冲区
    ring_buffer_t *rb = app_proto_acquire_buf(1, 2048);

    // 注册协议到多通道多协议解析模块
    s_rls_mask = app_proto_register(rls_probe_frame, rb);
    if (s_rls_mask == 0)
        return;

    // 绑定协议使用到的通道
    app_proto_bind_channel(s_rls_mask, CH_ID_RS485);

    g_rls_task_handle = osThreadNew(rls_handle_task, nullptr, &rls_task_attr);
}
sw_app_initcall(rls_module_init);
