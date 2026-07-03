/**
 * @file    dev_key.h
 * @brief   按键/拨码开关设备 — EXTI 按下捕获 + 轮询释放检测
 *
 * SW1/SW2/SW3 (PE10/11/12): EXTI 下降沿捕获按下，app_key 轮询确认释放
 * KEY_TST (PD8):           EXTI 下降沿释放信号量
 * DIP_SW1/SW2 (PE7/8):     纯轮询读取，无 EXTI
 *
 * 所有按键 GPIO 内部上拉 + 低有效（按下=0，释放=1）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/** @brief 按键/拨码开关 ID 枚举 */
typedef enum {
    DEV_KEY_SW1  = 0, /* PE12 干接点 1 */
    DEV_KEY_SW2  = 1, /* PE11 干接点 2 */
    DEV_KEY_SW3  = 2, /* PE10 干接点 3 */
    DEV_KEY_TST  = 3, /* PD8  TEST 按键 */
    DEV_KEY_DIP1 = 4, /* PE7   拨码开关 1 */
    DEV_KEY_DIP2 = 5, /* PE8   拨码开关 2 */
    DEV_KEY_COUNT
} dev_key_id_t;

/* ---- 硬件层初始化 (hw_dev_initcall) — 注册 EXTI 回调 ---- */
void dev_key_init(void);

/* ---- 软件层初始化 (sw_dev_initcall) — 创建 TEST 信号量 ---- */
void dev_key_sw_init(void);

/**
 * @brief 获取按键当前状态（EXTI 标记的瞬时值，不含去抖）
 * @param key_id  按键 ID
 * @return true  按下（低电平）
 * @return false 释放（高电平）
 *
 * SW1~SW3: EXTI 下降沿置 true，app_key 负责确认释放后置 false。
 * DIP1~2: 返回 app_key 更新后的去抖缓存。
 */
bool dev_key_get_state(dev_key_id_t key_id);

/**
 * @brief 清除 SW1~SW3 的按下标记（由 app_key 在确认释放后调用）
 */
void dev_key_clear_state(dev_key_id_t key_id);

/**
 * @brief 检查 TEST 按键是否被按下（非阻塞，取信号量）
 * @return true  TEST 按键在设备启动后被按下过
 *
 * 每次成功返回后信号量被消耗，再次按下前返回 false。
 */
bool dev_key_test_pressed(void);
