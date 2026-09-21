/**
 * @file    pl_exti_board.c
 * @brief   3833024 的 EXTI 中断向量
 *
 * 哪些向量存在、各自服务哪几根引脚是板级事实：3833024 有 TEST 键（PD8，落在
 * EXTI9_5）与 SW1~SW3（PE12/PE11/PE10，落在 EXTI15_10）。回调分派在共享的
 * pl_exti.c 里（HAL_GPIO_EXTI_Callback）。
 */

#include "pl_exti.h"
#include "main.h"

void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(KEY_TST_Pin);
}

void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(SW3_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW2_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW1_Pin);
}
