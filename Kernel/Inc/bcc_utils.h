#pragma once

/**
 * @file    bcc_utils.h
 * @brief   BCC 异或校验工具
 */

#include <stdint.h>
#include <stddef.h>

uint8_t bcc_calc(const uint8_t data[], uint16_t len);
