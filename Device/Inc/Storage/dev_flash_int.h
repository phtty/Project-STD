/**
 * @file    dev_flash_int.h
 * @brief   STM32 内部 Flash 存储设备基类 — 不持有实例
 *
 * 仅提供 dev_flash_int_t 派生类型和 g_flash_int_ops 虚表引用。
 * 实例由各自的配置模块创建（dev_flash_iap.c / dev_flash_ldi.c），
 * 在各自的 hw_dev_initcall 中绑定 ops。
 */

#pragma once

#include "dev_storage.h"
#include "pl_flash.h"

/** @brief 内部 Flash 派生类型（dev_storage_t 子类） */
typedef struct {
    dev_storage_t base; /**< 基类子对象，必须放在第一个成员位置 */
    uint32_t base_addr; /**< 本设备映射的内部 Flash 起始地址 */
    uint32_t sector;    /**< 擦除所用的内部 Flash 扇区号（PL_FLASH_SECTOR_*） */
} dev_flash_int_t;

/** @brief 共享的 g_flash_int_ops 虚表（定义在 dev_flash_int.c） */
extern const dev_storage_ops_t g_flash_int_ops;
