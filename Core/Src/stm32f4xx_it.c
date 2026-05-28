/**
 * @file    stm32f4xx_it.c
 * @brief   系统异常处理器（外设 ISR 已全部迁移至 Platform/Device 层）
 */

#include "main.h"
#include "stm32f4xx_it.h"

/* ---- HardFault 现场保护：崩溃信息存入全局变量，调试时 Watch 窗口查看 ---- */
static uint32_t g_fault_stack[8]; /* R0,R1,R2,R3,R12,LR,PC,xPSR */
static uint32_t g_fault_pc;       /* 崩溃指令地址 */
static uint32_t g_fault_cfsr;     /* SCB->CFSR 故障状态寄存器 */
static uint32_t g_fault_bfar;     /* SCB->BFAR 总线错误地址 */
static uint32_t g_fault_r0;       /* 故障时 R0 */

void hard_fault_handler_c(uint32_t *stack_frame)
{
    for (int i = 0; i < 8; i++)
        g_fault_stack[i] = stack_frame[i];
    g_fault_pc   = stack_frame[6];
    g_fault_r0   = stack_frame[0];
    g_fault_cfsr = SCB->CFSR;
    g_fault_bfar = SCB->BFAR;
    for (;;);
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "TST    LR, #4          \n"
        "ITE    EQ              \n"
        "MRSEQ  R0, MSP         \n"
        "MRSNE  R0, PSP         \n"
        "B      hard_fault_handler_c \n");
}

void NMI_Handler(void)
{
    while (1) {}
}

void MemManage_Handler(void)
{
    while (1) {}
}

void BusFault_Handler(void)
{
    while (1) {}
}

void UsageFault_Handler(void)
{
    while (1) {}
}

void DebugMon_Handler(void)
{
}
