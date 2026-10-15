/**
 * @file    app_cdstd_cmd.c
 * @brief   折叠屏创迪标准协议处理
 */

#include "app_cdstd.h"

#include "FreeRTOS.h"
#include "initcall.h"
#include "crc_utils.h"

#include "pl_net.h"
#include "pl_task.h"
#include "pl_mem.h"

#include "app_rs485.h"
#include "app_udp.h"
#include "app_cdstd_cmd.h"

/* ---- 队列和缓冲区资源静态分配 ---- */
#define CDSTD_PAYLOAD_MAX (512U)
#define CDSTD_MSG_SIZE    (sizeof(app_dispatch_msg_t) + CDSTD_PAYLOAD_MAX)

/*
 * 协议私有资源声明，包括帧消息队列、消息队列缓冲区、队列属性
 */
static StaticQueue_t s_cdstd_queue_cb;
static uint8_t s_cdstd_queue_buf[2 * CDSTD_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_cdstd_queue_attr = {
    .name    = "proto_cdstd_queue",
    .cb_mem  = &s_cdstd_queue_cb,
    .cb_size = sizeof(s_cdstd_queue_cb),
    .mq_mem  = s_cdstd_queue_buf,
    .mq_size = sizeof(s_cdstd_queue_buf),
};

static osMessageQueueId_t s_cdstd_msg_queue;
static osThreadId_t s_cdstd_task_handle;
static const osThreadAttr_t s_cdstd_task_attr = {
    .name       = "app_cdstd_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};
void app_cdstd_task(void *argument);

/*
 * 构造协议上下文资源，包括协议缓冲区、协议探针前向声明、协议控制块声明
 */
RB_DEFINE_ATTR(s_cdstd_rb, 1024, PL_CCMRAM);
app_pcb_probe_state_t app_cdstd_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src, uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len, uint8_t *aux);
static const app_pcb_ops_t s_cdstd_ops = {.probe = app_cdstd_probe_frame};
static app_pcb_t s_cdstd_pcb           = {
    .name        = "cdstd",
    .ops         = &s_cdstd_ops,
    .rb          = &s_cdstd_rb,
    .payload_max = CDSTD_PAYLOAD_MAX,
};

static_assert(CDSTD_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "CDSTD 最长帧超过框架暂存上限");

[[maybe_unused]] static void _cdstd_module_init(void)
{
    rb_init(&s_cdstd_rb, "cdstd");

    s_cdstd_msg_queue = osMessageQueueNew(2, CDSTD_MSG_SIZE, &s_cdstd_queue_attr);
    s_cdstd_pcb.queue = s_cdstd_msg_queue;

    app_dispatch_bind(&s_cdstd_pcb, app_udp_ccb());
    app_dispatch_bind(&s_cdstd_pcb, app_rs485_ccb());

    s_cdstd_task_handle = pl_task_new(app_cdstd_task, nullptr, &s_cdstd_task_attr);
}
sw_post_initcall(_cdstd_module_init);

/**
 * @brief 协议处理任务
 */
void app_cdstd_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[CDSTD_MSG_SIZE] = {0};
    app_dispatch_msg_t *msg                 = (app_dispatch_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_cdstd_msg_queue, msg, NULL, 100))
            continue;

        app_cdstd_frame_t *frame_data = (app_cdstd_frame_t *)msg->data;

        uint8_t cmd = msg->aux;
        if (cmd >= CDSTD_CMD_COUNT)
            continue;

        uint16_t len = (uint16_t)(frame_data->len[0] << 8) | frame_data->len[1];
        g_cdstd_cmd_table[cmd](msg->ccb, frame_data, len);
    }
}

/**
 * @brief   CDSTD 帧探测函数
 *
 * 检测 0xFFFF 帧头 → 校验 len 合法性 → CRC16 验证 → 返回完整帧长度
 * @retval APP_PCB_PROBE_STATE_READY  帧就绪
 * @retval APP_PCB_PROBE_STATE_WAIT   数据不足
 * @retval APP_PCB_PROBE_STATE_FAKE   伪帧头，跳过 1 字节重试
 * @retval APP_PCB_PROBE_STATE_SKIP   帧长超出暂存区，无法校验，交由框架整帧跳过
 */
app_pcb_probe_state_t app_cdstd_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src, uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len, uint8_t *aux)
{
    (void)src;
    (void)ccb;
    const ring_buffer_t *buff = self->rb;
    uint32_t available        = rb_avail(buff, nullptr);

    if (available < sizeof(app_cdstd_frame_t) + 2)
        return APP_PCB_PROBE_STATE_WAIT;

    uint16_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 2, nullptr);
    if (head != CDSTD_HEAD)
        return APP_PCB_PROBE_STATE_FAKE;

    uint8_t len[4]       = {0};
    uint16_t payload_len = 0;
    rb_peek(buff, 4, len, 4, nullptr);
    payload_len |= (uint16_t)(len[0] << 8);
    payload_len |= len[1];
    if (payload_len > 512)
        return APP_PCB_PROBE_STATE_FAKE;

    uint32_t full_bytes = (sizeof(app_cdstd_frame_t) + payload_len + 2);

    /* 整帧到齐之前不得窥视整帧：scratch 里还是**尚未写入**的旧内容，拿它比 CRC
       必然不过，分段到达的帧会被整批判成坏帧。 */
    if (available < full_bytes)
        return APP_PCB_PROBE_STATE_WAIT;

    if (full_bytes > scratch_size) {
        *total_len = full_bytes;
        return APP_PCB_PROBE_STATE_SKIP;
    }

    rb_peek(buff, 0, scratch, scratch_size, nullptr);
    app_cdstd_frame_t *ptemp = (app_cdstd_frame_t *)scratch;

    uint16_t crc = crc16_xmodem(scratch + 2, payload_len + 7);
    /* CRC 不过 = 伪帧/坏帧 → 跳 1 字节重探。**不能返回 SKIP** —— SKIP 会按
       total_len 把后面的字节一并吞掉，一条坏帧会连带丢掉它后面的真帧。 */
    if (crc != *(uint16_t *)(&(ptemp->data_crc[payload_len])))
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = full_bytes;
    *aux       = ptemp->cmd & 0x0F;
    return APP_PCB_PROBE_STATE_READY;
}