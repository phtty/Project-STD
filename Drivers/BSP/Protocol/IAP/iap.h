#pragma once

#include "main.h"

#include "protocol.h"
#include "RingBuff.h"

#define FRAME_MIN_LEN     (5U)
#define FRAME_MAX_LEN     (5U + 256U)

#define FRAME_HEAD        (0x5A5A5A5AU)
#define FRAME_HEAD_OFFSET (0U)
#define FRAME_SEQ_OFFSET  (1U)
#define FRAME_CMD_OFFSET  (2U)
#define FRAME_LEN_OFFSET  (3U)

typedef struct {
    uint32_t head;
    uint32_t seq;
    uint32_t cmd;
    uint32_t len;
    uint32_t data_crc[];
} iap_frame_t;

extern osMessageQueueId_t gx_IapQueue;
extern osThreadId_t g_iap_task_handle;
extern const osThreadAttr_t IapTask_attributes;

void iap_handle_task(void *argument);
proto_probe_sta_t iap_probe_frame(const ch_meta_t *meta, const RingBuff_t *buff, uint32_t *total_len, uint8_t *cmd_num);
