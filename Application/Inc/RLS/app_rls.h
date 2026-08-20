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
    RLS_CMD_DISPLAY_SW   = 0x5535,
    RLS_CMD_DISPLAY_TMP  = 0x4d42,
    RLS_CMD_DISPLAY_SAVE = 0x5653,
    RLS_CMD_DISPLAY_PIC  = 0x5344,
    RLS_CMD_INQUIRY      = 0x4b43,
    RLS_CMD_SET_IP       = 0x5049,
    RLS_CMD_AQUIRY_IP    = 0x5453,
    RLS_CMD_REPORT_IP    = 0x4252,
} rls_cmd_type_t;

extern osMessageQueueId_t g_rls_msg_queue;
extern osThreadId_t g_rls_task_handle;
extern const osThreadAttr_t rls_task_attr;
