/**
 * @file    pl_rtc.h
 * @brief   RTC 抽象接口（备份寄存器 + Unix 时间戳）
 */

#pragma once

#include <stdint.h>

/** @brief RTC 不透明句柄 */
typedef void *pl_rtc_handle_t;

void pl_rtc_init(void);
pl_rtc_handle_t pl_rtc_get_handle(void);

bool pl_rtc_bkup_write(pl_rtc_handle_t h, uint32_t reg, uint32_t value);
uint32_t pl_rtc_bkup_read(pl_rtc_handle_t h, uint32_t reg);
uint32_t pl_rtc_get_timestamp(pl_rtc_handle_t h);
bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts);
