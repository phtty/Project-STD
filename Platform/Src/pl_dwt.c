/**
 * @file    pl_dwt.c
 * @brief   DWT 周期计数器（ARM Cortex-M4 调试单元）
 */

#include "pl_dwt.h"
#include "stm32f4xx_hal.h"

void pl_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t pl_dwt_get_cycles(void)
{
    return DWT->CYCCNT;
}

void pl_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}
