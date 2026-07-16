#pragma once

#include <stdint.h>
#include <string.h>
#include "dev_storage.h"

// 地址与常量定义
#define ADDR_CONFIG_SECTOR  0x08004000
#define ADDR_RECOVERY_APP   0x08008000
#define ADDR_MAIN_APP       0x08040000

#define APP_FLASH_IAP_MAGIC 0x0d000721

// 网络配置
__attribute__((aligned(4))) typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint32_t port;
} app_flash_iap_net_cfg_t;

// main app 信息
__attribute__((aligned(4))) typedef struct {
    uint32_t size;  // Main App 字节长度
    uint32_t crc32; // Main App CRC32 值
    char version[32];
} app_flash_iap_fw_info_t;

// 升级状态机
typedef enum {
    APP_FLASH_IAP_UPDATED  = 0,
    APP_FLASH_IAP_UPDATING = 1,
    APP_FLASH_IAP_FAILED   = 2,
} app_flash_iap_update_sta_t;

// 存储在 0x08004000
__attribute__((aligned(4))) typedef struct {
    uint32_t magic;                   // 魔数，判断配置区是否有效
    uint32_t update_sta;              // 升级状态机
    app_flash_iap_fw_info_t app_info; // main_app 状态
    app_flash_iap_net_cfg_t net_cfg;  // 网络配置
    uint32_t config_crc;              // 本结构体自身的 CRC32 校验
} app_flash_iap_sys_info_t;

bool app_flash_iap_is_config_empty(volatile const app_flash_iap_sys_info_t *info);
bool app_flash_iap_is_config_valid(volatile const app_flash_iap_sys_info_t *info);
void app_flash_iap_init_config(app_flash_iap_sys_info_t *info);
void app_flash_iap_edit_config(app_flash_iap_sys_info_t *info);
int32_t app_flash_iap_erase_config(void);
int32_t app_flash_iap_write_config(app_flash_iap_sys_info_t *info);

/** @brief 同步设备IP/掩码/网关到内部Flash (LDI改IP时调用) */
void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);

/** @brief 获取 IAP Flash 存储句柄（内部使用） */
dev_storage_t *app_flash_iap_get_storage(void);

extern app_flash_iap_sys_info_t *g_config; /* 内存映射指针 (0x08004000) */
