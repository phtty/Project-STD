/**
 * @file    app_key.h
 * @brief   按键应用层 — 轮询去抖 + 状态缓存
 *
 * key_poll_task (20ms): 轮询 GPIO 实际电平与 dev_key EXTI 标记，
 * 完成释放去抖，更新本地状态缓存。
 */

#pragma once

#include <stdbool.h>
#include "dev_key.h"

/** @brief 应用层初始化 (sw_app_initcall) — 创建轮询任务 */
void app_key_init(void);

/**
 * @brief 获取按键去抖后的缓存状态
 * @param key_id  按键 ID
 * @return true  按下 / DIP 闭合
 * @return false 释放 / DIP 断开
 */
bool app_key_get_state(dev_key_id_t key_id);

/**
 * @brief 检查 TEST 按键按下事件（转发到 dev_key_test_pressed）
 */
bool app_key_test_pressed(void);

/** @brief 轮询任务入口（内部使用，不供外部直接调用） */
void app_key_task(void *argument);
