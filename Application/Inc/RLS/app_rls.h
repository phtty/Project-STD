#pragma once

#include "string.h"

#include "cmsis_os2.h"
#include "app_dispatch.h"

typedef struct {
    uint8_t head[2];
    uint8_t length[2];
    uint8_t cmd[2];
    uint8_t data_bcc_tail[];
} rls_frame_t;

typedef enum {
    RLS_CMD_TEST         = 0x0000,
    RLS_CMD_DISPLAY      = 0x4d42,
    RLS_CMD_DISPLAY_SAVE = 0x5653,
} rls_cmd_type_t;

extern osMessageQueueId_t g_rls_msg_queue;
extern osThreadId_t g_rls_task_handle;
extern const osThreadAttr_t rls_task_attr;

void rls_handle_task(void *argument);

/** @brief RLS 帧探测（pcb_ops.probe）—— 契约见 app_dispatch.h 的 pcb_probe_fn_t */
pcb_probe_sta_t rls_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux);
