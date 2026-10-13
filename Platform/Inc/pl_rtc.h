/**
 * @file    pl_rtc.h
 * @brief   RTC 抽象接口（备份寄存器 + Unix 时间戳）
 */

#pragma once

#include <stdint.h>

/** @brief RTC 不透明句柄 */
typedef void *pl_rtc_handle_t;

/** @brief 初始化 RTC 外设（备份域时钟、日历与备份寄存器访问） */
void pl_rtc_init(void);

/** @brief 取 RTC 外设句柄
 *  @return RTC 句柄（恒非 NULL） */
pl_rtc_handle_t pl_rtc_get_handle(void);

/** @brief 写 RTC 备份寄存器
 *  @param h     RTC 句柄
 *  @param reg   备份寄存器编号
 *  @param value 要写入的值
 *  @return true 写入完成（HAL 接口无失败路径） */
bool pl_rtc_bkup_write(pl_rtc_handle_t h, uint32_t reg, uint32_t value);

/** @brief 读 RTC 备份寄存器
 *  @param h   RTC 句柄
 *  @param reg 备份寄存器编号
 *  @return 该寄存器的当前值 */
uint32_t pl_rtc_bkup_read(pl_rtc_handle_t h, uint32_t reg);

/** @brief 读取当前 Unix 时间戳
 *  @param h RTC 句柄
 *  @return 当前 Unix 时间戳（秒） */
uint32_t pl_rtc_get_timestamp(pl_rtc_handle_t h);

/** @brief 设置 Unix 时间戳
 *  @param h  RTC 句柄
 *  @param ts 要设置的 Unix 时间戳（秒）
 *  @return true 成功，false 硬件返回错误 */
bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts);
