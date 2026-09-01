#include "app_rls.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "bcc_utils.h"
#include "app_rls_cmd.h"

/* RS485 通道 RB：与青海等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rs485, RB_SIZE_RS485);

/* ---- proto_rls_queue 静态分配 ---- */
#define RLS_PAYLOAD_MAX (530U) /* 帧头(6B) + bitmap(512B) + BCC(1B) + 尾(2B) + 余量 */
#define RLS_MSG_SIZE    (sizeof(frame_msg_t) + RLS_PAYLOAD_MAX)
#define RLS_QUEUE_DEPTH (2U)
_Static_assert(RLS_PAYLOAD_MAX <= RB_SIZE_RS485, "RLS max frame must fit RS485 RB");

static StaticQueue_t s_rls_queue_cb;
static uint8_t s_rls_queue_buf[RLS_QUEUE_DEPTH * RLS_MSG_SIZE];
static const osMessageQueueAttr_t s_rls_queue_attr = {
    .name    = "proto_rls_queue",
    .cb_mem  = &s_rls_queue_cb,
    .cb_size = sizeof(s_rls_queue_cb),
    .mq_mem  = s_rls_queue_buf,
    .mq_size = sizeof(s_rls_queue_buf),
};

static const rls_cmd_type_t cmd_index_table[] = {
    RLS_CMD_TEST,
    RLS_CMD_DISPLAY,
    RLS_CMD_DISPLAY_SAVE,
};

static proto_mask_t s_rls_mask;

osMessageQueueId_t g_rls_msg_queue;
osThreadId_t g_rls_task_handle;
const osThreadAttr_t rls_task_attr = {
    .name       = "rls_handle_task",
    .stack_size = 256 * 4, /* 帧缓冲为 static；原 2KB 偏大 */
    .priority   = (osPriority_t)osPriorityNormal,
};

/*--- 帧任务---*/
void rls_handle_task(void *argument)
{
    (void)argument;
    static uint8_t _msg_buf[RLS_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;
    g_rls_msg_queue  = osMessageQueueNew(RLS_QUEUE_DEPTH, RLS_MSG_SIZE, &s_rls_queue_attr);
    app_proto_set_frame_queue(s_rls_mask, g_rls_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_rls_msg_queue, msg, NULL, osWaitForever))
            continue;

        rls_frame_t *rls_frame = (rls_frame_t *)(msg->data);

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
static const uint8_t rls_head[2] = {0xFF, 0xFE};
static const uint8_t rls_tail[2] = {0x0D, 0x0C};
proto_probe_sta_t rls_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *aux)
{
    uint32_t avail = rb_avail(buff, nullptr);
    (void)ch;

    /* 首字节快速拒绝：禁止 avail<10 盲 WAIT 阻塞同 RB 链式探测 */
    if (avail == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(buff, 0, &first_byte, 1, nullptr);
    if (first_byte != rls_head[0])
        return PROTO_PROBE_FAKE;

    if (avail < 2)
        return PROTO_PROBE_WAIT;

    uint8_t second_byte;
    rb_peek(buff, 1, &second_byte, 1, nullptr);
    if (second_byte != rls_head[1])
        return PROTO_PROBE_FAKE;

    if (avail < sizeof(rls_frame_t) + 4)
        return PROTO_PROBE_WAIT;

    static uint8_t mem_pool[RLS_PAYLOAD_MAX] = {0};
    uint32_t peek_len = (avail > sizeof(mem_pool)) ? sizeof(mem_pool) : avail;
    memset(mem_pool, 0, sizeof(mem_pool));
    rb_peek(buff, 0, mem_pool, peek_len, nullptr);
    rls_frame_t *frame = (rls_frame_t *)mem_pool;

    if (memcmp(rls_head, frame->head, sizeof(rls_head)))
        return PROTO_PROBE_FAKE;

    uint16_t data_len = (frame->length[1] & 0xFF) | ((frame->length[0] << 8) & 0xFF00);

    /* 长度上下界：至少含头+尾，不超过已窥视/载荷上限 */
    if (data_len < (uint16_t)(sizeof(rls_frame_t) + 4) || data_len > peek_len ||
        data_len > RLS_PAYLOAD_MAX)
        return PROTO_PROBE_FAKE;

    if (memcmp(rls_tail, (uint8_t *)frame + data_len - 2, sizeof(rls_tail)))
        return PROTO_PROBE_FAKE;

    // 上位机的bcc校验没有做
    // uint8_t frame_bcc = ((uint8_t *)frame)[data_len - 3];
    // uint8_t calc_bcc  = bcc_calcu(frame->data_bcc_tail, data_len - sizeof(rls_frame_t) - 4);
    // if (frame_bcc != calc_bcc)
    //     return PROTO_PROBE_FAKE;

    *aux       = frame->cmd[0]; /* 命令高字节，供分发侧辅助区分 */
    *total_len = data_len;
    return PROTO_PROBE_READY;
}

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void rls_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RS485, RB_SIZE_RS485);
    if (rb == nullptr)
        return;

    s_rls_mask = app_proto_register(rls_probe_frame, rb);
    if (s_rls_mask == 0)
        return;

    app_proto_bind_channel(s_rls_mask, CH_ID_RS485);

    g_rls_task_handle = osThreadNew(rls_handle_task, nullptr, &rls_task_attr);
}
sw_app_initcall(rls_module_init);
