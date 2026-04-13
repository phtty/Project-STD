#include "iap.h"

#include "crc.h"

#include "udp_app.h"
#include "config_info.h"
#include "iap_cmd.h"

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osMessageQueueId_t gx_IapQueue;
osThreadId_t g_iap_task_handle;
const osThreadAttr_t IapTask_attributes = {
    .name       = "iap_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/**
 * @brief 将缓冲区中的数据进行协议解析
 *
 */
void iap_handle_task(void *argument)
{
    static frame_msg_t msg;
    const osMessageQueueAttr_t proto_iap_queue_attr = {
        .name = "g_proto_iap_queue",
    };
    gx_IapQueue                                = osMessageQueueNew(2, sizeof(frame_msg_t), &proto_iap_queue_attr);
    g_frame_queue[proto_index(PROTO_MASK_IAP)] = gx_IapQueue;

    for (;;) {
        // 等待数据接收任务发送队列
        if (osOK != osMessageQueueGet(gx_IapQueue, &msg, NULL, osWaitForever)) {
            continue;
        }

        iap_frame_t *frame_data = (iap_frame_t *)msg.data;
        uint8_t cmd             = (uint8_t)((frame_data->cmd) & 0xff);

        g_iap_cmd_table[cmd](&(msg.meta), frame_data);
    }
}

proto_probe_sta_t iap_probe_frame(const ch_meta_t *meta, const RingBuff_t *buff, uint32_t *total_len, uint8_t *cmd_num)
{
    uint32_t available = BSP_RB_GetAvailable(buff);
    (void)meta; // 未使用meta参数

    // 基础长度检查
    if (available < 4) return PROTO_PROBE_WAIT;

    // 帧头检查
    uint32_t head = 0;
    BSP_RB_PeekBlock(buff, 0, (uint8_t *)&head, 4);
    if (head != FRAME_HEAD)
        return PROTO_PROBE_FAKE; // 帧头不合法，标记为fake触发跳过

    // 长度合法性初步检查 (防止恶意大包或伪造包)
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        BSP_RB_PeekBlock(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4);

        if (payload_len > 256)
            return PROTO_PROBE_FAKE; // 协议规定最大256，超过范围则为伪造帧头

    } else {
        return PROTO_PROBE_WAIT; // 连长度字段都还没收到
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    // 数据完整性检查
    if (available < full_bytes) {
        // 如果在当前半截包的范围内发现了新帧头
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            BSP_RB_PeekBlock(buff, i, (uint8_t *)&next_head, 4);

            if (next_head == FRAME_HEAD)
                return PROTO_PROBE_FAKE;
        }
        return PROTO_PROBE_WAIT;
    }

    // CRC验证
    static uint32_t tmp_buff[FRAME_MAX_LEN];
    BSP_RB_PeekBlock(buff, 0, (uint8_t *)tmp_buff, full_bytes);
    iap_frame_t *ptemp = (iap_frame_t *)tmp_buff;

    uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)ptemp, ptemp->len + 4);
    if (crc != ptemp->data_crc[ptemp->len]) {
        return PROTO_PROBE_FAKE; // 长度够了但校验不过，判定为伪造或损坏
    }

    // 验证通过
    *total_len = full_bytes;
    *cmd_num   = ptemp->cmd & 0xFF;
    return PROTO_PROBE_READY;
}
