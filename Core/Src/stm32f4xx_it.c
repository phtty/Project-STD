/**
 * @file    stm32f4xx_it.c
 * @brief   系统异常处理器（外设 ISR 已全部迁移至 Platform/Device 层）
 */

#include "main.h"
#include "stm32f4xx_it.h"

void NMI_Handler(void)           { while (1) {} }
void HardFault_Handler(void)     { while (1) {} }
void MemManage_Handler(void)     { while (1) {} }
void BusFault_Handler(void)      { while (1) {} }
void UsageFault_Handler(void)    { while (1) {} }
void DebugMon_Handler(void)      {}
