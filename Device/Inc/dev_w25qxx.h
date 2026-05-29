/**
 * @file    dev_w25qxx.h
 * @brief   W25Qxx SPI Flash 存储设备（继承 dev_storage_t）
 */

#pragma once

#include "dev_storage.h"

/** @brief 获取全局 W25Qxx 存储设备实例（派生类型定义隐藏在 .c 中） */
dev_storage_t *dev_w25qxx_get(void);
