/**
 * @file    app_rs232.c
 * @brief   RS232 通道 Application 层 — 任务属性定义
 *
 * Project_STD 有两路 RS232：USART3 (RS232-0) 和 USART6 (RS232-1)
 */

#include "app_rs232.h"

const osThreadAttr_t rs232_0_task_attr = {
    .name       = "rs232_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

const osThreadAttr_t rs232_1_task_attr = {
    .name       = "rs232_1_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
