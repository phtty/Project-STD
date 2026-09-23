/**
 * @file    pl_dwt.h
 * @brief   DWT 周期计数器接口（Platform 层封装）
 */

#pragma once

#include <stdint.h>

/** @brief 使能 DWT 周期计数器（清零 CYCCNT） */
void     pl_dwt_init(void);

/** @brief 读取当前 DWT 周期计数值
 *  @return 自 pl_dwt_init 以来经过的 CPU 周期数 */
uint32_t pl_dwt_get_cycles(void);

/** @brief 毫秒级阻塞延时（封装 HAL_Delay）
 *  @param ms 延时长度（毫秒） */
void pl_delay_ms(uint32_t ms);
