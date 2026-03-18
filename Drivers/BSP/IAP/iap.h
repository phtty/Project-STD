#pragma once

#include "main.h"

#include "msg.h"
#include "RingBuff.h"

#define FRAME_MIN_LEN     (5U)
#define FRAME_MAX_LEN     (5U + 256U)

#define FRAME_HEAD        (0x5A5A5A5AU)
#define FRAME_HEAD_OFFSET (0U)
#define FRAME_SEQ_OFFSET  (1U)
#define FRAME_CMD_OFFSET  (2U)
#define FRAME_LEN_OFFSET  (3U)

typedef enum {
    iap_frame_rdy = 0, // 完美帧，可以解析
    iap_frame_wait,    // 拆包帧，帧头匹配，但数据没收齐，且后面没有新帧头
    iap_frame_fake,    // 伪造帧，帧头匹配，但CRC或长度不匹配
} IAP_FrameSta_t;

typedef struct iap_frame {
    uint32_t head;
    uint32_t seq;
    uint32_t cmd;
    uint32_t len;
    uint32_t data_crc[];
} IAP_Frame_t;

extern osThreadId_t IapHandle;
extern const osThreadAttr_t IapTask_attributes;

void IapTask(void *argument);
IAP_FrameSta_t check_frame_validity(const RingBuff_t *buff, uint32_t *total_len, uint8_t *cmd_num);
