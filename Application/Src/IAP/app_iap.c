/**
 * @file    app_iap.c
 * @brief   IAP 固件升级协议处理
 *
 * 帧格式: 0x5A5A5A5A (4B) | seq (4B) | cmd (4B) | len (4B) | data | CRC32 (4B)
 * 仅承载于 UDP（RJ45 共享 RB）；IAP 不再使用 RS485。
 */

#include "app_iap.h"
#include "FreeRTOS.h"
#include "initcall.h"
#include "pl_crc.h"
#include "app_udp.h"
#include "app_iap_cmd.h"

/* RJ45 物理通道 RB：与 LDI/MQTT 等同槽 weak 合并 */
RB_PROVIDE_WEAK(rb_provide_rj45, RB_SIZE_RJ45);

/* ---- proto_iap_queue 静态分配 ---- */
#define IAP_PAYLOAD_MAX (1044U) /* FRAME_MAX_LEN * 4 */
#define IAP_MSG_SIZE    (sizeof(frame_msg_t) + IAP_PAYLOAD_MAX)
#define IAP_QUEUE_DEPTH (2U)

static StaticQueue_t s_iap_queue_cb;
static uint8_t s_iap_queue_buf[IAP_QUEUE_DEPTH * IAP_MSG_SIZE];
static const osMessageQueueAttr_t s_iap_queue_attr = {
    .name    = "proto_iap_queue",
    .cb_mem  = &s_iap_queue_cb,
    .cb_size = sizeof(s_iap_queue_cb),
    .mq_mem  = s_iap_queue_buf,
    .mq_size = sizeof(s_iap_queue_buf),
};

/* ---- 协议模块自注册（仅 UDP → RJ45 槽，与 LDI 链式共享） ---- */
static proto_mask_t s_iap_mask_udp;

[[maybe_unused]] static void iap_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(RB_SLOT_RJ45, RB_SIZE_RJ45);
    if (rb == nullptr)
        return;

    s_iap_mask_udp = app_proto_register(iap_probe_frame, rb);
    if (s_iap_mask_udp == 0)
        return;

    app_proto_bind_channel(s_iap_mask_udp, CH_ID_UDP);

    g_iap_task_handle = osThreadNew(iap_handle_task, nullptr, &iap_task_attr);
}
sw_app_initcall(iap_module_init);

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osMessageQueueId_t g_iap_msg_queue;
osThreadId_t g_iap_task_handle;
const osThreadAttr_t iap_task_attr = {
    .name       = "iap_handle_task",
    .stack_size = 256 * 4, /* 帧缓冲为 static；原 2KB 偏大 */
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ================================================================
 *  任务实现
 * ================================================================ */

/** @brief IAP 协议处理任务：阻塞等待帧队列 → 按 cmd 字段查表分派到命令处理函数 */
void iap_handle_task(void *argument)
{
    (void)argument;
    static uint8_t _msg_buf[IAP_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;
    g_iap_msg_queue = osMessageQueueNew(IAP_QUEUE_DEPTH, IAP_MSG_SIZE, &s_iap_queue_attr);
    if (s_iap_mask_udp != 0)
        app_proto_set_frame_queue(s_iap_mask_udp, g_iap_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_iap_msg_queue, msg, NULL, osWaitForever))
            continue;

        iap_frame_t *frame_data = (iap_frame_t *)msg->data;
        uint8_t cmd             = (uint8_t)((frame_data->cmd) & 0xff);

        /* 查表越界防护：cmd 为 uint8_t 可达 255，须先校验小于表元素个数（IAP_CMD_COUNT，
         * 与 g_iap_cmd_table[] 定义处 static_assert 保持同步）。
         * 越界帧静默丢弃——IAP 协议无错误应答约定，无法回 NACK。 */
        if (cmd >= IAP_CMD_COUNT)
            continue;

        g_iap_cmd_table[cmd](msg->ch, frame_data);
    }
}

/**
 * @brief   IAP 帧探测函数
 *
 * 检测 0x5A5A5A5A 帧头 → 校验 len 合法性 (<=256) → CRC32 验证 → 返回完整帧长度
 * @retval PROBE_READY  帧就绪
 * @retval PROBE_WAIT   数据不足
 * @retval PROBE_FAKE   伪帧头，跳过 1 字节重试
 */
proto_probe_sta_t iap_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *total_len, uint8_t *cmd_num)
{
    uint32_t available = rb_avail(buff, nullptr);
    (void)ch;

    /* 首字节快速拒绝：IAP 帧头首字节固定 0x5A，禁止盲 WAIT 阻塞链式探测 */
    if (available == 0)
        return PROTO_PROBE_FAKE;

    uint8_t first_byte;
    rb_peek(buff, 0, &first_byte, 1, nullptr);
    if (first_byte != 0x5A)
        return PROTO_PROBE_FAKE;

    /* 首字节已匹配，后续不足 4 字节 → 真正 WAIT */
    if (available < 4)
        return PROTO_PROBE_WAIT;

    /* frame header check */
    uint32_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 4, nullptr);
    if (head != FRAME_HEAD)
        return PROTO_PROBE_FAKE;

    /* payload len sanity check (protocol max 256) */
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        rb_peek(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4, nullptr);
        if (payload_len > 256)
            return PROTO_PROBE_FAKE;
    } else {
        return PROTO_PROBE_WAIT;
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    /* consecutive header check: if insufficient data but another 0x5A in range, skip */
    if (available < full_bytes) {
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            rb_peek(buff, i, (uint8_t *)&next_head, 4, nullptr);
            if (next_head == FRAME_HEAD)
                return PROTO_PROBE_FAKE;
        }
        return PROTO_PROBE_WAIT;
    }

    /* CRC32 validation */
    static uint32_t tmp_buff[FRAME_MAX_LEN];
    rb_peek(buff, 0, (uint8_t *)tmp_buff, full_bytes, nullptr);
    iap_frame_t *ptemp = (iap_frame_t *)tmp_buff;

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)ptemp, (ptemp->len + 4) * 4);
    if (crc != ptemp->data_crc[ptemp->len])
        return PROTO_PROBE_FAKE;

    *total_len = full_bytes;
    *cmd_num   = ptemp->cmd & 0xFF;
    return PROTO_PROBE_READY;
}
