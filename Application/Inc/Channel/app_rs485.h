/**
 * @file    app_rs485.h
 * @brief   RS485 通道 Application 层桩（task 属性 + 创建）
 *
 * 通道实例和硬件初始化在 Device 层（dev_rs485），
 * Application 层通过 dev_rs485_get_channel() 获取实例，
 * 仅持有 RTOS 任务属性和创建逻辑。
 */

#pragma once

#include "cmsis_os2.h"
#include "dev_rs485.h"

extern const osThreadAttr_t rs485_task_attr;

/** @brief 启动 RS485 通道任务 */
static inline osThreadId_t app_rs485_start(void)
{
    return osThreadNew(uart_channel_task, dev_rs485_get_channel(), &rs485_task_attr);
}
