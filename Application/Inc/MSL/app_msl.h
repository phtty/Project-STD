#pragma once

#include "string.h"

#include "cmsis_os2.h"
#include "app_dispatch.h"

typedef struct {
    uint8_t head[2];
    uint8_t addr;
    uint8_t length[2];
    uint8_t cmd;
    uint8_t data_bcc[];
} msl_frame_t;

typedef enum {
    MSL_CMD_TEST       = 0x00,
    MSL_CMD_TEXT       = 0x01,
    MSL_CMD_BITMAP     = 0x02,
    MSL_CMD_FILL       = 0x03,
    MSL_CMD_LIGHTLEVEL = 0x04,
} msl_cmd_type_t;

extern uint8_t msl_addr;
extern osMutexId_t msl_tx_lock;
extern osMessageQueueId_t g_msl_msg_queue;
extern osThreadId_t g_msl_task_handle;
extern const osThreadAttr_t msl_task_attr;
