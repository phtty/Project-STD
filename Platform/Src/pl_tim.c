/**
 * @file        pl_tim.c
 * @brief       定时器初始化（hw_device_initcall 优先级 3）
 */

#include "pl_tim.h"
#include "tim.h"
#include "initcall.h"

void pl_tim_init(void)
{
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
}
hw_device_initcall(pl_tim_init);

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM7)
        HAL_IncTick();
}
