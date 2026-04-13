#include "config_info.h"

#include "crc.h"
#include "stm32f4xx_hal_flash_ex.h"

SysInfo_t *pConfig = (SysInfo_t *)ADDR_CONFIG_SECTOR;

/**
 * @brief 判断配置区是否为全空(0xFF)
 */
bool Is_Config_Empty(volatile const SysInfo_t *info)
{
    // 检查魔数和一部分关键内容是否为0xFF
    if (info->magic == 0xFFFFFFFF && info->config_crc == 0xFFFFFFFF) {
        return true;
    }
    return false;
}

bool Is_Config_Integrity(volatile const SysInfo_t *info)
{
    uint32_t crc32 = 0;

    if (info->magic != CONFIG_MAGIC)
        return false;

    crc32 = HAL_CRC_Calculate(&hcrc, (uint32_t *)info, (sizeof(SysInfo_t) - sizeof(info->config_crc)) / 4);
    if (info->config_crc != crc32)
        return false;

    return true;
}

/**
 * @brief 初始化config info并写入flash
 *
 * @param info config info结构体
 */
void Init_Config_Info(SysInfo_t *info)
{
    info->magic      = CONFIG_MAGIC; // 初始化魔数
    info->update_sta = failed;       // 初始化升级状态机

    memset(&(info->app_info), 0, sizeof(info->app_info)); // 初始化固件信息

    NetConfig_t net_info = {
        // 初始化网络配置信息
        .ip   = {192, 168, 114, 200},
        .mask = {255, 255, 255, 0},
        .gw   = {192, 168, 114, 1},
        .port = 0x2538,
    };
    memcpy(&(info->net_cfg), &net_info, sizeof(NetConfig_t));

    // 计算config info的crc校验值
    info->config_crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)info, (sizeof(SysInfo_t) - sizeof(info->config_crc)) / 4);

    // 擦除config info所在扇区并将数据写入
    EraseConfigInfo();
    WriteConfigInfo(info);
}

/**
 * @brief 修改config info并写入flash
 *
 * @param info config info结构体
 */
void Edit_Config_Info(SysInfo_t *info)
{
    // 计算config info的crc校验值
    info->config_crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)info, (sizeof(SysInfo_t) - sizeof(info->config_crc)) / 4);

    // 擦除config info所在扇区并将数据写入
    EraseConfigInfo();
    WriteConfigInfo(info);
}

/**
 * @brief 擦除flash中的config info
 *
 * @return HAL_StatusTypeDef 操作结果
 */
HAL_StatusTypeDef EraseConfigInfo(void)
{
    HAL_StatusTypeDef status               = HAL_ERROR;
    uint32_t SectorError                   = 0;
    FLASH_EraseInitTypeDef EraseInitStruct = {0};

    HAL_FLASH_Unlock(); // 解锁flash
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    EraseInitStruct.TypeErase    = FLASH_TYPEERASE_SECTORS;
    EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3; // 电压范围2.7V~3.6V
    EraseInitStruct.Sector       = FLASH_SECTOR_1;        // 获取扇区号
    EraseInitStruct.NbSectors    = 1;
    HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError);

    if (EraseInitStruct.Sector != FLASH_SECTOR_11) // 防止地址越界
        status = HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError);

    HAL_FLASH_Lock();
    return status;
}

/**
 * @brief 将config info写入flash
 *
 * @param info config info结构体
 * @return HAL_StatusTypeDef 操作结果
 */
HAL_StatusTypeDef WriteConfigInfo(SysInfo_t *info)
{
    HAL_StatusTypeDef status = HAL_ERROR;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    // 按word将config info写入flash
    for (uint32_t i = 0; i < sizeof(SysInfo_t) / 4; i++) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ADDR_CONFIG_SECTOR + i * 4, ((uint32_t *)info)[i]);

        if (status != HAL_OK) {
            break; // 写入出错，退出
        }
    }

    HAL_FLASH_Lock();
    return status;
}
