/**
 * @file    pl_adc.h
 * @brief   ADC 平台层抽象（环境光传感器）
 */

#pragma once

#include <stdint.h>

typedef void *pl_adc_handle_t;

void     pl_adc_init(void);
pl_adc_handle_t pl_adc_get_handle(void);
int32_t  pl_adc_read(pl_adc_handle_t h, uint32_t *value, uint32_t timeout_ms);
