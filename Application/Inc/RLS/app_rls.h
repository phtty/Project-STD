#pragma once

/**
 * @file    app_rls.h
 * @brief   RLS 协议：帧格式、命令类型与任务接口
 */

#include "string.h"

#include "cmsis_os2.h"
#include "app_dispatch.h"

typedef struct {
    uint8_t head[2];
    uint8_t length[2];
    uint8_t cmd[2];
    uint8_t data_bcc_tail[];
} app_rls_frame_t;

typedef enum {
    APP_RLS_CMD_TYPE_TEST         = 0x0000,
    APP_RLS_CMD_TYPE_DISPLAY      = 0x4d42,
    APP_RLS_CMD_TYPE_DISPLAY_SAVE = 0x5653,
} app_rls_cmd_type_t;

extern osMessageQueueId_t g_rls_msg_queue;
extern osThreadId_t g_rls_task_handle;
extern const osThreadAttr_t g_rls_task_attr;

void app_rls_task(void *argument);

/** @brief RLS 帧探测（pcb_ops.probe）—— 契约见 app_dispatch.h 的 app_pcb_probe_fn_t */
app_pcb_probe_state_t app_rls_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux);
