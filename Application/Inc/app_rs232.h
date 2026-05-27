/**
 * @file    app_rs232.h
 * @brief   RS232 通道 Application 层桩（task 属性 + 创建）
 */

#pragma once

#include "cmsis_os2.h"
#include "dev_rs232.h"

extern const osThreadAttr_t rs232_0_task_attr;
extern const osThreadAttr_t rs232_1_task_attr;

/** @brief 启动 RS232-0 通道任务 (USART3) */
static inline osThreadId_t app_rs232_start(void)
{
    return osThreadNew(uart_channel_task, dev_rs232_get_channel(0), &rs232_0_task_attr);
}

/** @brief 启动 RS232-1 通道任务 (USART6) — Project_STD 独有 */
static inline osThreadId_t app_rs232_1_start(void)
{
    return osThreadNew(uart_channel_task, dev_rs232_get_channel(1), &rs232_1_task_attr);
}
