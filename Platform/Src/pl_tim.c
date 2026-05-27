/**
 * @file        pl_tim.c
 * @brief       定时器初始化（hw_device_initcall 优先级 3）
 *
 * Project_STD 使用 TIM2/3/4/7。
 * HAL_TIM_PeriodElapsedCallback 保留在 main.c（需要 dev_display）。
 */

#include "pl_tim.h"
#include "tim.h"
#include "initcall.h"

void pl_tim_init(void)
{
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_TIM4_Init();
}
hw_device_initcall(pl_tim_init);
