/**
 * @file    app_rs232.h
 * @brief   RS232 通道 Application 层桩（task 属性 + 创建）
 */

#pragma once

#include "cmsis_os2.h"
#include "dev_rs232.h"

extern const osThreadAttr_t rs232_task_attr;

static inline osThreadId_t app_rs232_start(void)
{
    return osThreadNew(uart_channel_task, dev_rs232_get_channel(), &rs232_task_attr);
}
