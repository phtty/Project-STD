/**
 * @file    pl_crc.h
 * @brief   CRC32 硬件抽象（STM32 硬件 CRC 单元）
 *
 * 多项式 0x4C11DB7（与 MPEG2/Ethernet 一致）。
 * 输入自动对齐，对齐数据零拷贝直接透传硬件。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/** @brief CRC 不透明句柄 */
typedef void *pl_crc_handle_t;

void            pl_crc_init(void);
pl_crc_handle_t pl_crc_get_handle(void);

/** @brief 硬件 CRC32 计算 */
uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len);
