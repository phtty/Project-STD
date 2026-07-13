/**
 * @file    app_rs232.h
 * @brief   RS232 通道 Application 层
 */
#pragma once

#include "cmsis_os2.h"

osThreadId_t app_rs232_start(void);
osThreadId_t app_rs232_1_start(void);
