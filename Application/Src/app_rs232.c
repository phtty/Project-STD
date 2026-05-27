/**
 * @file    app_rs232.c
 * @brief   RS232 通道 Application 层 — 任务属性定义
 */

#include "app_rs232.h"

const osThreadAttr_t rs232_task_attr = {
    .name       = "rs232_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
