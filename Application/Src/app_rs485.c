/**
 * @file    app_rs485.c
 * @brief   RS485 通道 Application 层 — 任务属性定义
 */

#include "app_rs485.h"

const osThreadAttr_t rs485_task_attr = {
    .name       = "rs485_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
