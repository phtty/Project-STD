/**
 * @file    pl_exti_board.c
 * @brief   5005048 的 EXTI 中断向量
 *
 * 5005048 只有测试键（PD8，落在 EXTI9_5）。3833024 额外的 SW1~SW3（EXTI15_10）
 * 在本板不存在，故没有 EXTIn_5_10 的向量实现。
 * 回调分派在共享的 pl_exti.c 里（HAL_GPIO_EXTI_Callback）。
 */

#include "pl_exti.h"
#include "main.h"

void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(KEY_TST_Pin);
}
