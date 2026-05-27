/**
 * @file    pl_dwt.h
 * @brief   DWT 周期计数器接口（Platform 层封装）
 */

#pragma once

#include <stdint.h>

void     pl_dwt_init(void);
uint32_t pl_dwt_get_cycles(void);
void     pl_delay_ms(uint32_t ms);    /* Platform 层毫秒延时（封装 HAL_Delay） */
