/**
 * @file    app_factory_test.h
 * @brief   出厂检测模式 — TEST 按键驱动状态机
 *
 * 状态流程：
 *   IDLE ──[TEST]──▶ SHOW_CODE ──[TEST]──▶ DEAD_PIXEL ──[TEST×7]──▶ AGING ──[TEST]──▶ 退出
 */

#pragma once

#include <stdint.h>

/** @brief 终止工厂测试模式 */
void app_factory_mode_interrupt(void);

/** @brief 远程注入一次 TEST 按键事件（IAP 0x0C 调用） */
void app_factory_test_trigger(void);

/** @brief 接收事件监听入口：放行 IAP 0x00004B0C，其余帧仍退出工厂测试 */
void app_factory_mode_rx_notify(const char *proto, uint32_t cmd);
