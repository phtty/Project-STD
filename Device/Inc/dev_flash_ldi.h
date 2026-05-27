#pragma once

#include "main.h"

// Flash Sector 11: 0x080E0000 ~ 0x080FFFFF, 128KB
// 存放 LDI 车道设备接口协议配置信息
#define ADDR_LDI_CONFIG_SECTOR 0x080E0000
#define DEV_FLASH_LDI_MAGIC    0x0D001B00
#define DEV_FLASH_LDI_MAX_MODULES (6U) // Flash 最大可存储的功能模块数量

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
} dev_flash_ldi_module_cfg_t;

static_assert(sizeof(dev_flash_ldi_module_cfg_t) == 12);

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
    dev_flash_ldi_module_cfg_t modules[DEV_FLASH_LDI_MAX_MODULES]; // N <= MAX_MODULES
} dev_flash_ldi_cfg_info_t;

/**
 * Flash 存储记录 = magic + 配置 + CRC32 校验
 * 总长 116 字节 (含编译器对齐 padding), 按 word 写入 Flash
 */
typedef struct {
    uint32_t magic;
    dev_flash_ldi_cfg_info_t cfg;
    uint32_t crc32;
} dev_flash_ldi_record_t;

bool dev_flash_ldi_is_config_empty(volatile const dev_flash_ldi_record_t *rec);
bool dev_flash_ldi_is_config_valid(volatile const dev_flash_ldi_record_t *rec);
int32_t dev_flash_ldi_erase_config(void);
int32_t dev_flash_ldi_write_config(dev_flash_ldi_record_t *rec);
void dev_flash_ldi_save_config(dev_flash_ldi_cfg_info_t *info);
void dev_flash_ldi_load_config(dev_flash_ldi_cfg_info_t *info);
