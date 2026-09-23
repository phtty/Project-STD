/**
 * @file    pl_adc.h
 * @brief   ADC 平台层抽象（环境光传感器）
 */

#pragma once

#include <stdint.h>

/** @brief ADC 不透明句柄 */
typedef void *pl_adc_handle_t;

/** @brief 初始化 ADC 外设（环境光通道） */
void     pl_adc_init(void);

/** @brief 取 ADC 外设句柄
 *  @return ADC 句柄（恒非 NULL） */
pl_adc_handle_t pl_adc_get_handle(void);

/** @brief 启动一次转换并读取结果
 *  @param h          ADC 句柄
 *  @param[out] value 接收转换结果；本函数写入 12 位采样值
 *  @param timeout_ms 轮询等待转换完成的超时（毫秒）
 *  @return 0 成功，-1 参数非法或超时 */
int32_t  pl_adc_read(pl_adc_handle_t h, uint32_t *value, uint32_t timeout_ms);
