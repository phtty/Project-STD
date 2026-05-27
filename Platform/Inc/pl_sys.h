/**
 * @file    pl_sys.h
 * @brief   系统基础接口：时钟配置、复位、全局 HAL MSP 初始化（Platform 层）
 *
 * 合并自 pl_clock.h / pl_system.h / pl_msp.c，
 * 统一管理芯片级基础功能，均为 Platform 层对外暴露的单一职责接口。
 */

#pragma once

/* ---- 系统时钟：HSE → PLL → 168MHz SYSCLK (APB1=42MHz, APB2=84MHz) ---- */
void SystemClock_Config(void);

/* ---- 系统复位：软件复位 MCU ---- */
void pl_system_reset(void);
