#pragma once

/**
 * @file    bcc_utils.h
 * @brief   BCC 异或校验工具
 */

#include <stdint.h>
#include <stddef.h>

/** @brief 计算 BCC（逐字节异或）校验值
 *  @param data 待校验数据
 *  @param len  数据长度（字节）
 *  @return 异或校验结果 */
uint8_t bcc_calc(const uint8_t data[], uint16_t len);
