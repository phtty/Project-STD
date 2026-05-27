/**
 * @file    pl_adc.c
 * @brief   ADC 平台层实现（封装 hadc1）
 */

#include "pl_adc.h"
#include "adc.h"
#include "initcall.h"

void pl_adc_init(void)
{
    MX_ADC1_Init();
}
hw_device_initcall(pl_adc_init);

pl_adc_handle_t pl_adc_get_handle(void)
{
    return (pl_adc_handle_t)&hadc1;
}

int32_t pl_adc_read(pl_adc_handle_t h, uint32_t *value, uint32_t timeout_ms)
{
    ADC_HandleTypeDef *hadc = (ADC_HandleTypeDef *)h;
    if (!hadc || !value) return -1;

    HAL_ADC_Start(hadc);
    if (HAL_ADC_PollForConversion(hadc, timeout_ms) != HAL_OK) {
        HAL_ADC_Stop(hadc);
        return -1;
    }
    *value = HAL_ADC_GetValue(hadc);
    HAL_ADC_Stop(hadc);
    return 0;
}
