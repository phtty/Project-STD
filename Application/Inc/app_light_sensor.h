/**
 * @file    app_light_sensor.h
 * @brief   环境光传感器 RTOS 任务 — 周期性读取 ADC 并自动调节显示亮度
 */

#pragma once

#include "cmsis_os2.h"
#include "dev_light_sensor.h"

extern osThreadId_t g_light_sensor_task_handle;

void app_light_sensor_init(void);
void app_light_sensor_task(void *argument);
