#pragma once

#include <stdint.h>
#include "dev_storage.h"

// Flash Sector 11 (0x080E0000~0x080FFFFF, 128KB) 已释放。
// LDI 配置现已迁移至 W25Qxx 最后一个 4KB 扇区，通过 dev_storage_ops 访问。
#define APP_FLASH_LDI_MAGIC    0x0D001B00
#define APP_FLASH_LDI_MAX_MODULES (6U) // Flash 最大可存储的功能模块数量

/**
 * 单功能模块配置信息（从 0BH 命令下发，共 12 字节）
 *
 * 每个模块包含设备类型编码、模块序号和厂商自定义参数段。
 * 存储在 Flash 中供 1EH 参数采集命令回读。
 */
typedef struct {
    uint8_t device_type;   // ldi_device_t 枚举值 (E1H~EBH)
    uint8_t device_index;  // 功能模块序号，从 01H 开始
    uint8_t vendor[10];    // 厂商自定义参数段 (0BH 命令下发的 Vendor[] 段)
} app_flash_ldi_module_cfg_t;

static_assert(sizeof(app_flash_ldi_module_cfg_t) == 12);

/**
 * LDI 车道设备配置信息 (共 46 字节)
 *
 * 存储 0AH 指令下发的全部网络参数 + 0BH 下发的模块配置。
 * 设备上电后从中加载配置。
 */
typedef struct {
    uint8_t device_ip[4];  // 设备自身 IP 地址
    uint16_t device_port;  // 设备端口号
    uint8_t host_ip[4];    // 上位机 IP 地址 (外设控制服务)
    uint16_t host_port;    // 上位机端口号
    uint8_t netmask[4];    // 子网掩码
    uint8_t gateway[4];    // 网关地址
    uint8_t lane_hex[5];   // 车道 HEX 编号 (来自 req_head.lane_code)
    uint8_t cert[8];       // 设备验证信息 (来自 req_head.cert_info)
    uint8_t module_count;  // 功能模块数量 N
    app_flash_ldi_module_cfg_t modules[APP_FLASH_LDI_MAX_MODULES]; // N <= MAX_MODULES
} app_flash_ldi_cfg_info_t;

/**
 * Flash 存储记录 = magic + 配置 + CRC32 校验
 * 总长 116 字节 (含编译器对齐 padding), 按 word 写入 Flash
 */
typedef struct {
    uint32_t magic;
    app_flash_ldi_cfg_info_t cfg;
    uint32_t crc32;
} app_flash_ldi_record_t;

bool app_flash_ldi_is_config_empty(volatile const app_flash_ldi_record_t *rec);
bool app_flash_ldi_is_config_valid(volatile const app_flash_ldi_record_t *rec);
int32_t app_flash_ldi_erase_config(void);
int32_t app_flash_ldi_write_config(app_flash_ldi_record_t *rec);
void app_flash_ldi_save_config(app_flash_ldi_cfg_info_t *info);
/** @brief 从 W25Qxx 加载 LDI 配置，返回 true 表示读取到有效配置 */
bool app_flash_ldi_load_config(app_flash_ldi_cfg_info_t *info);

/** @brief 获取 LDI Flash 存储句柄（内部使用） */
dev_storage_t *app_flash_ldi_get_storage(void);
