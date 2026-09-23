#pragma once

#include <stdint.h>
#include "cmsis_os2.h"

#include "app_dispatch.h"
#include "ring_buffer.h"

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
} app_iap_frame_t;

/** @brief 取 IAP 协议控制块
 *
 *  供板级代码把"本板才有的通道"绑到 IAP 上（如 3833024 的两路 RS232）。
 *  共享的 app_iap.c 只绑定两块板都有的通道（RS485 / UDP），板级特有的通道由
 *  各板自己绑——否则共享文件要认识每块板的外设。 */
app_pcb_t *app_iap_pcb(void);

extern osMessageQueueId_t g_iap_msg_queue;
extern osThreadId_t g_iap_task_handle;
extern const osThreadAttr_t g_iap_task_attr;

void app_iap_task(void *argument);

/** @brief IAP 帧探测（pcb_ops.probe）—— 契约见 app_dispatch.h 的 app_pcb_probe_fn_t */
app_pcb_probe_state_t app_iap_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux);
