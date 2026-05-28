/**
 * @file    dev_w25qxx.h
 * @brief   W25Qxx SPI Flash 存储设备（继承 dev_storage_t）
 */

#pragma once

#include "dev_storage.h"

typedef struct {
    dev_storage_t dev;          /* 必须是第一个成员 */
    void *spi;                  /* pl_spi_handle_t */
} dev_w25qxx_t;

/** @brief 获取全局 W25Qxx 存储设备实例 */
dev_storage_t *dev_w25qxx_get(void);

/** @brief 初始化（sw_device_initcall 自动调用，也可手动调用） */
void dev_w25qxx_init(void);
