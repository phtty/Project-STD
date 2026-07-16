/**
 * @file    app_factory_test.h
 * @brief   出厂检测模式 — TEST 按键驱动状态机
 *
 * 状态流程：
 *   IDLE ──[TEST]──▶ SHOW_CODE ──[TEST]──▶ DEAD_PIXEL ──[TEST×7]──▶ AGING ──[TEST]──▶ 退出
 */

#pragma once

/** @brief 终止工厂测试模式 */
void app_factory_mode_interrupt(void);
