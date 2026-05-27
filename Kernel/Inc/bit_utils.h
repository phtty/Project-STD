/**
 * @file    bit_utils.h
 * @brief   芯片无关的位操作
 */

#pragma once

#include <stdint.h>

/** @brief 计算 uint32_t 二进制尾部零的数量（替代 GCC __builtin_ctz） */
uint8_t bit_ctz(uint32_t x);
