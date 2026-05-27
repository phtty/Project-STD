/**
 * @file        pl_tim.c
 * @brief       定时器初始化（hw_device_initcall 优先级 3）
 *
 * Project_STD 使用 TIM2/3/4/7。
 * HAL_TIM_PeriodElapsedCallback 保留在 main.c（需要 dev_display）。
 */

#include "pl_tim.h"
#include "tim.h"

extern TIM_HandleTypeDef htim7;
#include "initcall.h"

void pl_tim_init(void)
{
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_TIM4_Init();
}
hw_device_initcall(pl_tim_init);

/* ---- TIM ISR (从 stm32f4xx_it.c 迁移) ---- */
void TIM2_IRQHandler(void) { HAL_TIM_IRQHandler(&htim2); }
void TIM3_IRQHandler(void) { HAL_TIM_IRQHandler(&htim3); }
void TIM4_IRQHandler(void) { HAL_TIM_IRQHandler(&htim4); }
void TIM7_IRQHandler(void) { HAL_TIM_IRQHandler(&htim7); }
