#pragma once

#include <stdint.h>

/* LDI 配置持久化 — 存在 W25Qxx 尾部的配置区内，由配置调度器统一管理
 * （记录名 "ldi_cfg"）。归属/地址/版本/长度/CRC32 都在记录头里，本模块只管载荷语义。
 *
 * 历史：曾自行定义 magic + cfg + crc32 的 116 字节记录并写死在"容量−4KB"扇区；
 * 迁移到调度器后那些字段与地址计算都由框架接管。 */

/** 记录格式版本。载荷布局变更时 +1 —— 版本不符会被判为记录失效、回落默认值，
 *  而不是让记录搬家（见 app_cfg_sched.c 的扫描认领）。 */
#define APP_FLASH_LDI_VERSION (1U)

#define APP_FLASH_LDI_MAX_MODULES (6U) // 可存储的功能模块数量上限

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
 * LDI 车道设备配置信息（共 46 字节）
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

/** @brief 从配置区加载 LDI 配置，返回 true 表示读到有效配置
 *  @note  首次调用会触发一次读取，之后走缓存（见 .c 里的幂等说明） */
bool app_flash_ldi_load_config(app_flash_ldi_cfg_info_t *info);

/** @brief 保存 LDI 配置到配置区
 *  @return 0 成功（含内容未变而去重跳过）；负值失败
 *  @note  返回值必须检查：此前是 void，保存失败在现场只表现为
 *         "配好了、重启又变回去"，无从查起 */
int32_t app_flash_ldi_save_config(const app_flash_ldi_cfg_info_t *info);
