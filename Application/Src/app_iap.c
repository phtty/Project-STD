/**
 * @file    app_iap.c
 * @brief   IAP 固件升级协议处理
 *
 * 帧格式: 0x5A5A5A5A (4B) | seq (4B) | cmd (4B) | len (4B) | data | CRC32 (4B)
 * 承载于 RS485 + UDP，配置存储于 Flash 0x08004000。
 */

#include "app_iap.h"
#include "initcall.h"
#include "pl_crc.h"
#include "app_udp.h"
#include "dev_flash_iap.h"
#include "app_iap_cmd.h"

/* ---- 协议模块自注册 ---- */
static void iap_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(0, 2048);
    app_proto_register(PROTO_MASK_IAP, iap_probe_frame, rb);
    app_proto_bind_channel(PROTO_MASK_IAP, CH_ID_RS485);
    app_proto_bind_channel(PROTO_MASK_IAP, CH_ID_UDP);
    g_iap_task_handle = osThreadNew(iap_handle_task, nullptr, &iap_task_attr);
}
sw_device_initcall(iap_module_init);

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osMessageQueueId_t g_iap_msg_queue;
osThreadId_t g_iap_task_handle;
const osThreadAttr_t iap_task_attr = {
    .name       = "iap_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ================================================================
 *  任务实现
 * ================================================================ */

/** @brief IAP 协议处理任务：阻塞等待帧队列 → 按 cmd 字段查表分派到命令处理函数 */
void iap_handle_task(void *argument)
{
    static frame_msg_t msg;
    const osMessageQueueAttr_t proto_iap_queue_attr = {
        .name = "proto_iap_queue",
    };
    g_iap_msg_queue = osMessageQueueNew(2, sizeof(frame_msg_t), &proto_iap_queue_attr);
    app_proto_set_frame_queue(PROTO_MASK_IAP, g_iap_msg_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_iap_msg_queue, &msg, NULL, osWaitForever))
            continue;

        iap_frame_t *frame_data = (iap_frame_t *)msg.data;
        uint8_t cmd             = (uint8_t)((frame_data->cmd) & 0xff);

        g_iap_cmd_table[cmd](msg.ch, frame_data);
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

    /* min size check */
    if (available < 4) return PROTO_PROBE_WAIT;

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
