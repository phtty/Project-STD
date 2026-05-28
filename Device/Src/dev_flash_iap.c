/**
 * @file    dev_flash_iap.c
 * @brief   IAP 系统配置 Flash 存储（Sector 1, 0x08004000）
 *
 * dev_flash_iap_sys_info_t 记录 = magic(4B) | update_sta(4B) | FWInfo(40B) | NetConfig(16B) | CRC32(4B)
 * 总长 68B（17 words），按 word 写入 Flash。
 *
 * 操作流程：
 *   init/edit → 填充结构体 → 计算 CRC32 → erase → write
 *   读取 → 直接内存映射 (ADDR_CONFIG_SECTOR) → 空/完整性检查 → 使用
 */

#include "dev_flash_iap.h"
#include "dev_flash_int.h"
#include "pl_crc.h"

dev_flash_iap_sys_info_t *g_config = (dev_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR; /* 直接映射到 Flash 地址 */

/* ================================================================
 *  完整性检查
 * ================================================================ */

/** @brief Flash 擦除后全为 0xFF，magic + crc 均为 0xFFFFFFFF 表示从未写入 */
bool dev_flash_iap_is_config_empty(volatile const dev_flash_iap_sys_info_t *info)
{
    return info->magic == 0xFFFFFFFF && info->config_crc == 0xFFFFFFFF;
}

/** @brief magic 匹配后校验 CRC32（覆盖整个结构体除去 crc32 字段自身） */
[[maybe_unused]] static bool dev_flash_iap_is_config_valid(volatile const dev_flash_iap_sys_info_t *info)
{
    if (info->magic != DEV_FLASH_IAP_MAGIC) return false;

    size_t len     = sizeof(dev_flash_iap_sys_info_t) - sizeof(info->config_crc);
    uint32_t crc32 = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, len);
    return info->config_crc == crc32;
}

/* ================================================================
 *  写入操作（先擦除整扇区，再逐 word 编程）
 * ================================================================ */

/** @brief 初始化配置：设置 magic + 默认 IP(192.168.114.200:9529) + 空固件信息，写入 Flash */
void dev_flash_iap_init_config(dev_flash_iap_sys_info_t *info)
{
    info->magic      = DEV_FLASH_IAP_MAGIC;
    info->update_sta = DEV_FLASH_IAP_FAILED;
    memset(&(info->app_info), 0, sizeof(info->app_info));

    dev_flash_iap_net_cfg_t net_info = {
        .ip   = {192, 168, 114, 200},
        .mask = {255, 255, 255, 0},
        .gw   = {192, 168, 114, 1},
        .port = 0x2538,
    };
    memcpy(&(info->net_cfg), &net_info, sizeof(dev_flash_iap_net_cfg_t));

    /* CRC32 覆盖 magic + update_sta + app_info + net_cfg，不含 crc32 自身 */
    info->config_crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, sizeof(dev_flash_iap_sys_info_t) - sizeof(info->config_crc));

    dev_flash_iap_erase_config();
    dev_flash_iap_write_config(info);
}

/** @brief 修改配置：更新 CRC32 → 擦除 → 写入 */
void dev_flash_iap_edit_config(dev_flash_iap_sys_info_t *info)
{
    info->config_crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, sizeof(dev_flash_iap_sys_info_t) - sizeof(info->config_crc));
    dev_flash_iap_erase_config();
    dev_flash_iap_write_config(info);
}

/** @brief 擦除 Sector 1（0x08004000，16KB） */
int32_t dev_flash_iap_erase_config(void)
{
    return dev_storage_erase(dev_flash_int_get(DEV_FLASH_INT_IAP), 0, 0);
}

/** @brief 写入 dev_flash_iap_sys_info_t 到 Flash */
int32_t dev_flash_iap_write_config(dev_flash_iap_sys_info_t *info)
{
    return dev_storage_write(dev_flash_int_get(DEV_FLASH_INT_IAP), 0, (uint8_t *)info, sizeof(*info));
}
