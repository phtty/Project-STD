#include "app_msl.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "bcc_utils.h"
#include "app_msl_cmd.h"
#include "app_key.h"

/* ---- proto_msl_queue 静态分配 ---- */
#define MSL_PAYLOAD_MAX (1032U) /* 帧头(2B) + 地址(1B) + 长度(2B) + 命令(1B) + 数据(1024B) + BCC(1B) */
#define MSL_MSG_SIZE    (sizeof(frame_msg_t) + MSL_PAYLOAD_MAX)

static StaticQueue_t s_msl_queue_cb;
static uint8_t s_msl_queue_buf[2 * MSL_MSG_SIZE];
static const osMessageQueueAttr_t s_msl_queue_attr = {
    .name    = "proto_msl_queue",
    .cb_mem  = &s_msl_queue_cb,
    .cb_size = sizeof(s_msl_queue_cb),
    .mq_mem  = s_msl_queue_buf,
    .mq_size = sizeof(s_msl_queue_buf),
};

static proto_mask_t s_msl_mask;

osMessageQueueId_t g_msl_msg_queue;
osThreadId_t g_msl_task_handle;
const osThreadAttr_t msl_task_attr = {
    .name       = "msl_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

uint8_t msl_addr = 0;

/* ---- 主卡 UART6 发送互斥锁：保护 msl_render_* 的共享缓冲 + 阻塞发送 ---- */
osMutexId_t msl_tx_lock;

/*--- 帧任务---*/
void msl_handle_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[MSL_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;
    g_msl_msg_queue  = osMessageQueueNew(1, MSL_MSG_SIZE, &s_msl_queue_attr);
    app_proto_set_frame_queue(s_msl_mask, g_msl_msg_queue);

    // 通过拨码开关检测
    msl_addr = (dev_key_get_state(DEV_KEY_DIP1) & 0b01) | ((dev_key_get_state(DEV_KEY_DIP2) << 1) & 0b10);

    for (;;) {
        if (osOK != osMessageQueueGet(g_msl_msg_queue, msg, NULL, osWaitForever))
            continue;

        msl_frame_t *msl_frame = (msl_frame_t *)(msg->data);

        g_msl_cmd_table[msl_frame->cmd](msg->ch, msl_frame->data_bcc);
    }
}

/* ---- 帧探测 ---- */
static const uint8_t frame_head[2] = {0x5a, 0x5a};
proto_probe_sta_t msl_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *aux)
{
    (void)ch;

    uint32_t avail = rb_avail(buff, nullptr);
    // 只需完整帧头即可解析长度；原门槛 sizeof(msl_frame_t)+4 (10B) 会卡死 8B 亮度帧，
    // 使其躺在缓冲区等下一帧才被解析 → 副卡亮度永远滞后一个采样周期
    if (avail < sizeof(msl_frame_t))
        return PROTO_PROBE_WAIT;

    static uint8_t mem_pool[MSL_MSG_SIZE] = {0};
    memset(mem_pool, 0, sizeof(mem_pool));
    // 封顶 peek 长度，防止缓冲区数据量超过 mem_pool 容量时越界
    uint32_t peek_len = avail > sizeof(mem_pool) ? sizeof(mem_pool) : avail;
    rb_peek(buff, 0, mem_pool, peek_len, nullptr);
    msl_frame_t *frame = (msl_frame_t *)mem_pool;

    if (memcmp(frame_head, frame->head, sizeof(frame->head)))
        return PROTO_PROBE_FAKE;

    uint16_t data_len = (frame->length[1] & 0xFF) | ((frame->length[0] << 8) & 0xFF00);

    // 长度越界视为伪帧，防止线路噪声伪造帧头堵塞缓冲区
    if (data_len > MSL_PAYLOAD_MAX - sizeof(msl_frame_t) - 1)
        return PROTO_PROBE_FAKE;

    // 完整帧必须到齐才解析，避免半帧被当伪帧逐字节丢弃
    if (avail < data_len + sizeof(msl_frame_t) + 1)
        return PROTO_PROBE_WAIT;

    // 通过拨码开关检测
    msl_addr = (dev_key_get_state(DEV_KEY_DIP1) & 0b01) | ((dev_key_get_state(DEV_KEY_DIP2) << 1) & 0b10);
    // 地址不匹配：帧结构合法但不是发给本卡的，整帧跳过（不逐字节试探）
    if ((frame->addr != 0) && (frame->addr != msl_addr)) {
        *total_len = data_len + sizeof(msl_frame_t) + 1;
        return PROTO_PROBE_SKIP;
    }

    // bcc校验
    uint8_t frame_bcc = (frame->data_bcc)[data_len];
    uint8_t calc_bcc  = bcc_calcu(&(frame->addr), data_len + sizeof(msl_frame_t) - 2);
    if (frame_bcc != calc_bcc)
        return PROTO_PROBE_FAKE;

    (void)*aux;
    *total_len = data_len + sizeof(msl_frame_t) + 1;
    return PROTO_PROBE_READY;
}

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void msl_module_init(void)
{
    // 通过拨码开关检测
    msl_addr = (dev_key_get_state(DEV_KEY_DIP1) & 0b01) | ((dev_key_get_state(DEV_KEY_DIP2) << 1) & 0b10);

    // 主卡 UART6 发送互斥锁（优先级继承，防低优先级持锁饿死）
    const osMutexAttr_t msl_tx_lock_attr = {.name = "msl_tx_lock", .attr_bits = osMutexPrioInherit};
    msl_tx_lock = osMutexNew(&msl_tx_lock_attr);

    // 指定协议使用的环形缓冲区
    ring_buffer_t *rb = app_proto_acquire_buf(1, 2048);

    // 注册协议到多通道多协议解析模块
    s_msl_mask = app_proto_register(msl_probe_frame, rb);
    if (s_msl_mask == 0)
        return;

    // 绑定协议使用到的通道
    app_proto_bind_channel(s_msl_mask, CH_ID_RS232_1);

    g_msl_task_handle = osThreadNew(msl_handle_task, nullptr, &msl_task_attr);
}
sw_app_initcall(msl_module_init);
