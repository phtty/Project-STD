/**
 * @file    pl_sys.h
 * @brief   系统基础接口：时钟配置、复位、全局 HAL MSP 初始化（Platform 层）
 *
 * 合并自 pl_clock.h / pl_system.h / pl_msp.c，
 * 统一管理芯片级基础功能，均为 Platform 层对外暴露的单一职责接口。
 */

#pragma once

#include <stdint.h>

/* ---- 系统时钟：HSE → PLL → 168MHz SYSCLK (APB1=42MHz, APB2=84MHz) ---- */
/** @brief 配置系统时钟树到 168MHz（HSE → PLL） */
void SystemClock_Config(void);

/* ---- 阻塞延时（毫秒） ---- */
/** @brief 毫秒级阻塞延时
 *  @param ms 延时长度（毫秒） */
void pl_delay_ms(uint32_t ms);

/* ---- 系统复位：软件复位 MCU ---- */
/** @brief 触发 MCU 软件复位（不返回） */
void pl_system_reset(void);
