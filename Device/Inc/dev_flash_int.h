/**
 * @file    dev_flash_int.h
 * @brief   STM32 内部 Flash 存储设备基类 — 不持有实例
 *
 * 仅提供 dev_flash_int_t 派生类型和 flash_int_ops 虚表引用。
 * 实例由各自的配置模块创建（dev_flash_iap.c / dev_flash_ldi.c），
 * 在各自的 hw_dev_initcall 中绑定 ops。
 */

#pragma once

#include "dev_storage.h"
#include "pl_flash.h"

/** @brief 内部 Flash 派生类型（dev_storage_t 子类） */
typedef struct {
    dev_storage_t me;
    uint32_t base_addr;
    uint32_t sector;
} dev_flash_int_t;

/** @brief 共享的 flash_int_ops 虚表（定义在 dev_flash_int.c） */
extern const dev_storage_ops_t flash_int_ops;
