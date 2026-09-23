/**
 * @file    app_light_sensor.h
 * @brief   环境光传感器 RTOS 任务 — 周期性读取 ADC 并自动调节显示亮度
 */

#pragma once

#include "cmsis_os2.h"
#include "dev_light_sensor.h"

extern osThreadId_t g_light_sensor_task_handle; /**< 环境光任务句柄 */

/** @brief 初始化环境光传感器并创建调光任务（sw_app initcall 调用） */
void app_light_sensor_init(void);

/** @brief 环境光任务：1s 周期读取 ADC 并自动调节亮度（仅主卡生效）
 *  @param argument 未使用（单例任务） */
void app_light_sensor_task(void *argument);

/** @brief 停止环境光跟随, 使用指定亮度 (0=关屏, 1~7=固定亮度) */
void app_light_sensor_set_fixed(uint8_t level);

/** @brief 立即恢复环境光跟随 (马上调光一次, 无需等待 1s 周期) */
void app_light_sensor_resume(void);
