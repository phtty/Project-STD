#include "iap.h"

#include "crc.h"

#include "udp_app.h"
#include "config_info.h"
#include "iap_cmd.h"

uint8_t ptcl_buff[FRAME_MAX_LEN * 4] = {0};

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osThreadId_t IapHandle;
const osThreadAttr_t IapTask_attributes = {
    .name       = "IapTask",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/**
 * @brief 将缓冲区中的数据进行协议解析
 *
 */
void IapTask(void *argument)
{
    MsgQueueItem_t q_item;

    for (;;) {
        // 等待数据接收任务发送队列
        osMessageQueueGet(gx_PacketMsg, &q_item, NULL, osWaitForever);

        // 解析缓冲区的数据直到缓冲区有效内容长度小于1字节
        while (BSP_RB_GetAvailable(&xIAP_RB) >= 4) {
            uint32_t frame_len = 0;
            uint8_t cmd_num    = 0;

            IAP_FrameSta_t status = check_frame_validity(&xIAP_RB, &frame_len, &cmd_num);

            if (status == iap_frame_rdy) {
                BSP_RB_GetByte_Bulk(&xIAP_RB, ptcl_buff, frame_len);
                pfIAP_CMD[cmd_num](&q_item, (IAP_Frame_t *)ptcl_buff);

            } else if (status == iap_frame_wait) { // 数据包不完整
                break;

            } else { // 确认为脏数据或伪造包，滑动1字节继续查找
                BSP_RB_SkipBytes(&xIAP_RB, 1);
            }
        }
    }
}

/**
 * @brief 检查帧格式
 *
 * @param buff 环形缓冲区指针
 * @param total_len 帧长度
 * @param cmd_num 识别到的指令码内容
 * @return true 合法帧
 * @return false 非法帧
 */
IAP_FrameSta_t check_frame_validity(const RingBuff_t *buff, uint32_t *total_len, uint8_t *cmd_num)
{
    uint32_t available = BSP_RB_GetAvailable(buff);

    // 基础长度检查
    if (available < 4)
        return iap_frame_wait;

    // 帧头检查
    uint32_t head = 0;
    BSP_RB_PeekBlock(buff, 0, (uint8_t *)&head, 4);
    if (head != FRAME_HEAD)
        return iap_frame_fake; // 开头就不是5A，直接标记为fake触发跳过

    // 长度合法性初步检查 (防止恶意大包或伪造包)
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        BSP_RB_PeekBlock(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4);

        if (payload_len > 256)
            return iap_frame_fake; // 协议规定最大256，超过范围则为伪造帧头

    } else {
        return iap_frame_wait; // 连长度字段都还没收到
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    // 数据完整性检查
    if (available < full_bytes) {
        // 如果在当前半截包的范围内发现了新帧头
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            BSP_RB_PeekBlock(buff, i, (uint8_t *)&next_head, 4);

            if (next_head == FRAME_HEAD)
                return iap_frame_fake;
        }
        return iap_frame_wait;
    }

    // CRC最终验证
    static uint32_t tmp_buff[FRAME_MAX_LEN];
    BSP_RB_PeekBlock(buff, 0, (uint8_t *)tmp_buff, full_bytes);
    IAP_Frame_t *ptemp = (IAP_Frame_t *)tmp_buff;

    uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)ptemp, ptemp->len + 4);
    if (crc != ptemp->data_crc[ptemp->len]) {
        return iap_frame_fake; // 长度够了但校验不过，判定为伪造或损坏
    }

    // 验证通过
    *total_len = full_bytes;
    *cmd_num   = ptemp->cmd & 0xFF;
    return iap_frame_rdy;
}
