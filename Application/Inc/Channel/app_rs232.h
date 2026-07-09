/**
 * @file    app_rs232.h
 * @brief   RS232 通道 Application 层
 */
#pragma once

#include "cmsis_os2.h"

extern const osThreadAttr_t rs232_0_task_attr;
extern const osThreadAttr_t rs232_1_task_attr;

osThreadId_t app_rs232_start(void);
osThreadId_t app_rs232_1_start(void);
