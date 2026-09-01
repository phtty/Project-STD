/**
 * @file    app_factory_test.h
 * @brief   出厂检测模式 — TEST 按键驱动状态机
 *
 * 状态流程：
 *   IDLE ──[TEST]──▶ SHOW_CODE ──[TEST]──▶ DEAD_PIXEL ──[TEST×7]──▶ OBLIQUE_SCAN ──[TEST]──▶ AGING ──[TEST]──▶ 退出
 */

#pragma once

#include <stdbool.h>

/** @brief 业务数据到达 → 中止工厂测试当前序列，monitor 回 IDLE 待机（不销毁任务，TEST 键保持可用） */
void app_factory_mode_interrupt(void);

/**
 * @brief  查询工厂测试是否处于激活状态（进入检测序列 true，回 IDLE false）。
 * @note   供心跳类定时任务判定是否暂停计数（工厂测试占用显示资源）。
 */
bool app_factory_test_active(void);
