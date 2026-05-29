/**
 * @file    dev_flash_int.h
 * @brief   STM32 内部 Flash 存储设备（继承 dev_storage_t）
 *
 * 一个模块，两个实例：Sector1(IAP=16KB) 和 Sector11(LDI=128KB)
 */

#pragma once

#include "dev_storage.h"

#define DEV_FLASH_INT_IAP 0
#define DEV_FLASH_INT_LDI 1

dev_storage_t *dev_flash_int_get(uint8_t id);
