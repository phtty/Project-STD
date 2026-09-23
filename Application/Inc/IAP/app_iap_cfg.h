#pragma once

/**
 * @file    app_iap_cfg.h
 * @brief   IAP 配置持久化：Flash 地址常量、系统信息记录与读写接口
 */

#include <stdint.h>
#include <string.h>
#include "dev_storage.h"

// 地址与常量定义
#define ADDR_CONFIG_SECTOR  0x08004000 /**< IAP 配置记录所在扇区（Sector 1）基址 */
#define ADDR_RECOVERY_APP   0x08008000 /**< Recovery 应用映像基址 */
#define ADDR_MAIN_APP       0x08040000 /**< Main 应用映像基址 */

#define APP_FLASH_IAP_MAGIC 0x0d000721 /**< 配置记录有效时的魔数 */

/** @brief 网络配置（IP / 掩码 / 网关 / 端口） */
__attribute__((aligned(4))) typedef struct {
    uint8_t ip[4];      /**< 设备 IP 地址 */
    uint8_t mask[4];    /**< 子网掩码 */
    uint8_t gw[4];      /**< 默认网关 */
    uint32_t port;      /**< 设备端口号 */
} app_flash_iap_net_cfg_t; /**< 网络配置类型 */

/** @brief main app 固件信息（字节长度 / CRC32 / 版本） */
__attribute__((aligned(4))) typedef struct {
    uint32_t size;    /**< Main App 字节长度 */
    uint32_t crc32;   /**< Main App CRC32 值 */
    char version[32]; /**< 版本字符串 */
} app_flash_iap_fw_info_t; /**< main app 固件信息类型 */

/** @brief 升级状态机 */
typedef enum {
    APP_FLASH_IAP_UPDATED  = 0, /**< 已更新完成 */
    APP_FLASH_IAP_UPDATING = 1, /**< 更新进行中 */
    APP_FLASH_IAP_FAILED   = 2, /**< 更新失败 */
} app_flash_iap_update_status_t;

/** @brief IAP 系统信息记录（固化于 0x08004000） */
__attribute__((aligned(4))) typedef struct {
    uint32_t magic;                   /**< 魔数，判断配置区是否有效 */
    uint32_t update_status;           /**< 升级状态机 */
    app_flash_iap_fw_info_t app_info; /**< main_app 状态 */
    app_flash_iap_net_cfg_t net_cfg;  /**< 网络配置 */
    uint32_t config_crc;              /**< 本结构体自身的 CRC32 校验 */
} app_flash_iap_sys_info_t; /**< IAP 系统信息记录类型 */

/** @brief 判断配置记录是否为从未写入的空记录
 *  @param info 记录指针（只读）
 *  @return true 表示 magic 与 CRC 均为 0xFFFFFFFF（Flash 擦除后未写入） */
bool app_flash_iap_is_config_empty(volatile const app_flash_iap_sys_info_t *info);

/** @brief 校验配置记录是否有效（magic 匹配且 CRC32 一致）
 *  @param info 记录指针（只读）
 *  @return true 表示记录完整可用 */
bool app_flash_iap_is_config_valid(volatile const app_flash_iap_sys_info_t *info);

/** @brief 初始化配置：置 magic + 默认网络参数(192.168.114.200:9529) + 空固件信息，写入 Flash
 *  @param[out] info 待填充并落盘的记录
 *  @note 本板无 IAP 记录区时写路径整体短路，函数变为无操作 */
void app_flash_iap_init_config(app_flash_iap_sys_info_t *info);

/** @brief 修改配置：更新 CRC32 → 擦除 → 写入
 *  @param[in,out] info 记录；函数会就地刷新其 config_crc 字段 */
void app_flash_iap_edit_config(app_flash_iap_sys_info_t *info);

/** @brief 擦除 Sector 1（0x08004000，16KB）
 *  @return 0 成功，负值错误码 */
int32_t app_flash_iap_erase_config(void);

/** @brief 写入 app_flash_iap_sys_info_t 到 Flash
 *  @param info 待写入的记录（只读，不修改其内容）
 *  @return 0 成功，负值错误码 */
int32_t app_flash_iap_write_config(app_flash_iap_sys_info_t *info);

/** @brief 同步设备网络配置到内部 Flash（读-改-net_cfg-写，不碰其他字段）
 *
 *  三种情形：空记录 → 以有效骨架初始化后覆盖（见下）；损坏记录 → 拒绝覆盖；
 *  内容未变 → 跳过擦除。
 *
 *  **空记录那条是修过的 bug**：旧版对空记录直接走进来，把 `info`（整块 0xFF）
 *  连同改过的 net_cfg 写回去，magic 仍为 0xFFFFFFFF —— 产出的是"非空但无效"的
 *  记录，Bootloader 下次上电可能判定不进入主程序。全新设备收到一次 0AH 就会中招。
 *
 *  @param ip   设备 IP 地址（4 字节）
 *  @param mask 子网掩码（4 字节）
 *  @param gw   默认网关（4 字节）
 *  @param port 设备端口号 */
void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port);

/** @brief 镜像同步：把运行态的网络参数写进 IAP 记录（供 Recovery 上报）
 *
 *  IAP 记录 net_cfg 的唯一职责就是**镜像 main app 当前使用的网络参数**。
 *  由 pl_net 的 IP 变更监听触发，以及上电对账一次。 */
void app_flash_iap_sync_from_runtime(void);

/** @brief 获取 IAP Flash 存储句柄（内部使用）
 *  @return 存储设备句柄 */
dev_storage_t *app_flash_iap_get_storage(void);

extern app_flash_iap_sys_info_t *g_iap_sys_info; /**< 记录区内存映射指针 (0x08004000) */
