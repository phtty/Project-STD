/**
 * @file    pl_flash.h
 * @brief   内部 Flash 操作接口（Platform 层封装）
 *
 * 封装 STM32 HAL Flash API，通过枚举隔离芯片特有的扇区号和电压范围。
 * Device 层只传语义标识，不感知 HAL 宏。
 */

#pragma once

#include <stdint.h>

/** @brief Flash 扇区标识（隔离 HAL 的 FLASH_SECTOR_x 宏） */
typedef enum {
    PL_FLASH_SECTOR_0 = 0,
    PL_FLASH_SECTOR_1,
    PL_FLASH_SECTOR_2,
    PL_FLASH_SECTOR_3,
    PL_FLASH_SECTOR_4,
    PL_FLASH_SECTOR_5,
    PL_FLASH_SECTOR_6,
    PL_FLASH_SECTOR_7,
    PL_FLASH_SECTOR_8,
    PL_FLASH_SECTOR_9,
    PL_FLASH_SECTOR_10,
    PL_FLASH_SECTOR_11,
} pl_flash_sector_t;

/** @brief Flash 操作电压范围（隔离 HAL 的 FLASH_VOLTAGE_RANGE_x 宏） */
typedef enum {
    PL_FLASH_VOLTAGE_1 = 0,  /* 1.8V ~ 2.1V */
    PL_FLASH_VOLTAGE_2,      /* 2.1V ~ 2.7V */
    PL_FLASH_VOLTAGE_3,      /* 2.7V ~ 3.6V */
    PL_FLASH_VOLTAGE_4,      /* 2.7V ~ 3.6V + External Vpp */
} pl_flash_voltage_t;

void     pl_flash_unlock(void);
void     pl_flash_lock(void);
void     pl_flash_clear_errors(void);
int32_t  pl_flash_erase_sector(pl_flash_sector_t sector, pl_flash_voltage_t range);
int32_t  pl_flash_program_word(uint32_t addr, uint32_t data);
