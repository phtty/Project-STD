/**
 * @file    pl_flash.c
 * @brief   内部 Flash 操作 Platform 层封装
 */

#include "main.h"
#include "pl_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

void pl_flash_unlock(void)
{
    HAL_FLASH_Unlock();
}

void pl_flash_lock(void)
{
    HAL_FLASH_Lock();
}

void pl_flash_clear_errors(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

/* 电压范围枚举 → HAL 值映射 */
static const uint32_t g_voltage_map[] = {
    [PL_FLASH_VOLTAGE_1] = FLASH_VOLTAGE_RANGE_1,
    [PL_FLASH_VOLTAGE_2] = FLASH_VOLTAGE_RANGE_2,
    [PL_FLASH_VOLTAGE_3] = FLASH_VOLTAGE_RANGE_3,
    [PL_FLASH_VOLTAGE_4] = FLASH_VOLTAGE_RANGE_4,
};

int32_t pl_flash_erase_sector(pl_flash_sector_t sector, pl_flash_voltage_t range)
{
    FLASH_EraseInitTypeDef cfg = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .VoltageRange = g_voltage_map[range],
        .Sector       = (uint32_t)sector,
        .NbSectors    = 1,
    };
    uint32_t err = 0;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&cfg, &err);
    return (st == HAL_OK) ? 0 : -1;
}

int32_t pl_flash_program_word(uint32_t addr, uint32_t data)
{
    HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
    return (st == HAL_OK) ? 0 : -1;
}
