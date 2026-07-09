/**
 * @file    app_rs485.h
 * @brief   RS485 通道 Application 层
 */
#pragma once

#include "cmsis_os2.h"

extern const osThreadAttr_t rs485_task_attr;

osThreadId_t app_rs485_start(void);
