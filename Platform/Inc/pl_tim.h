/**
 * @file    pl_tim.h
 * @brief   定时器抽象（Platform 层）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/** @brief 定时器实例 ID */
enum {
    PL_TIM2 = 0,
    PL_TIM3,
    PL_TIM4,
    PL_TIM7,
    PL_TIM_MAX,
};

typedef void *pl_tim_handle_t;

void pl_tim_init(void);
pl_tim_handle_t pl_tim_get_handle(uint8_t id);

/** @brief 启动定时器中断 */
void pl_tim_start_it(pl_tim_handle_t h);

/** @brief NVIC 中断开关（用于 OE 原子操作等） */
void pl_tim_irq_disable(uint8_t irq);
void pl_tim_irq_enable(uint8_t irq);

/** @brief 调试冻结定时器 */
void pl_tim_dbg_freeze(pl_tim_handle_t h);
