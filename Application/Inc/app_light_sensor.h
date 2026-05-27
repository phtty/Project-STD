/**
 * @file    app_light_sensor.h
 * @brief   环境光传感器 RTOS 任务（依赖注入 light_sensor_dev_t + display_dev_t）
 */

#pragma once

#include "dev_light_sensor.h"

void app_light_sensor_task(void *argument);
