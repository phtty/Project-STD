/**
 * @file    app_iap_cfg.c
 * @brief   IAP 系统配置 Flash 存储（Sector 1, 0x08004000）
 *
 * app_flash_iap_sys_info_t 记录 = magic(4B) | update_sta(4B) | FWInfo(40B) | NetConfig(16B) | CRC32(4B)
 * 总长 68B（17 words），按 word 写入 Flash。
 *
 * 操作流程：
 *   init/edit → 填充结构体 → 计算 CRC32 → erase → write
 *   读取 → 直接内存映射 (ADDR_CONFIG_SECTOR) → 空/完整性检查 → 使用
 */

#include "app_iap_cfg.h"
#include "dev_flash_int.h"
#include "pl_crc.h"
#include "pl_net.h"
#include "app_tcp_server.h"
#include "initcall.h"

#define IAP_SIZE 0x4000 /* 16KB */

/* IAP ABI 记录必须适配 Sector 1（16KB 单一擦除单元，与 Bootloader 共享布局）。
   记录长度是 A/B 两侧的接口契约：本地 .c 改了结构体而 Bootloader 侧未同步，
   唯一的表现是上电读不出配置。把这条钉在编译期。 */
_Static_assert(sizeof(app_flash_iap_sys_info_t) <= 16U * 1024U, "IAP ABI record exceeds sector 1");

/* ---- IAP Flash 存储实例 ---- */
static dev_flash_int_t g_flash_iap = {
    .me        = {.capacity = IAP_SIZE},
    .base_addr = ADDR_CONFIG_SECTOR,
    .sector    = PL_FLASH_SECTOR_1,
};

dev_storage_t *app_flash_iap_get_storage(void)
{
    return &g_flash_iap.me;
}

/* ---- ops 绑定（hw_dev_initcall） ---- */
static void _app_flash_iap_storage_init(void)
{
    g_flash_iap.me.ops = &flash_int_ops;
}
hw_dev_initcall(_app_flash_iap_storage_init);

app_flash_iap_sys_info_t *g_config = (app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR; /* 直接映射到 Flash 地址 */

/* ================================================================
 *  完整性检查
 * ================================================================ */

/** @brief Flash 擦除后全为 0xFF，magic + crc 均为 0xFFFFFFFF 表示从未写入 */
bool app_flash_iap_is_config_empty(volatile const app_flash_iap_sys_info_t *info)
{
    return info->magic == 0xFFFFFFFF && info->config_crc == 0xFFFFFFFF;
}

/** @brief magic 匹配后校验 CRC32（覆盖整个结构体除去 crc32 字段自身） */
bool app_flash_iap_is_config_valid(volatile const app_flash_iap_sys_info_t *info)
{
    if (info->magic != APP_FLASH_IAP_MAGIC) return false;

    size_t len     = sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc);
    uint32_t crc32 = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, len);
    return info->config_crc == crc32;
}

/* ================================================================
 *  写入操作（先擦除整扇区，再逐 word 编程）
 * ================================================================ */

/** @brief 初始化配置：设置 magic + 默认 IP(192.168.114.200:9529) + 空固件信息，写入 Flash */
void app_flash_iap_init_config(app_flash_iap_sys_info_t *info)
{
    info->magic      = APP_FLASH_IAP_MAGIC;
    info->update_sta = APP_FLASH_IAP_FAILED;
    memset(&(info->app_info), 0, sizeof(info->app_info));

    app_flash_iap_net_cfg_t net_info = {
        .ip   = {192, 168, 114, 200},
        .mask = {255, 255, 255, 0},
        .gw   = {192, 168, 114, 1},
        .port = 0x2538,
    };
    memcpy(&(info->net_cfg), &net_info, sizeof(app_flash_iap_net_cfg_t));

    /* CRC32 覆盖 magic + update_sta + app_info + net_cfg，不含 crc32 自身 */
    info->config_crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc));

    app_flash_iap_erase_config();
    app_flash_iap_write_config(info);
}

/** @brief 修改配置：更新 CRC32 → 擦除 → 写入 */
void app_flash_iap_edit_config(app_flash_iap_sys_info_t *info)
{
    info->config_crc = pl_crc32_calc(pl_crc_get_handle(), (uint8_t *)info, sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc));
    app_flash_iap_erase_config();
    app_flash_iap_write_config(info);
}

/** @brief 擦除 Sector 1（0x08004000，16KB） */
int32_t app_flash_iap_erase_config(void)
{
    return dev_storage_erase(app_flash_iap_get_storage(), 0, 0);
}

/** @brief 写入 app_flash_iap_sys_info_t 到 Flash */
int32_t app_flash_iap_write_config(app_flash_iap_sys_info_t *info)
{
    return dev_storage_write(app_flash_iap_get_storage(), 0, (uint8_t *)info, sizeof(*info));
}

/** @brief 同步设备网络配置到内部 Flash（读-改-net_cfg-写，不碰其他字段）
 *
 *  空记录：以有效骨架初始化后覆盖 net_cfg —— 旧版对空记录直接写会产出
 *  magic 仍为 0xFFFFFFFF 的**永久无效记录**（update_sta 一并置 UPDATED，
 *  保证 Bootloader 下次上电判定进入主程序）；
 *  损坏记录：拒绝覆盖；内容未变：跳过擦除（内部 Flash 擦除会硬停总线）。 */
void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    app_flash_iap_sys_info_t info;
    memcpy(&info, (void *)ADDR_CONFIG_SECTOR, sizeof(info));

    if (app_flash_iap_is_config_empty(&info)) {
        /* 空记录：初始化有效骨架，net_cfg 随后覆盖 */
        info.magic      = APP_FLASH_IAP_MAGIC;
        info.update_sta = APP_FLASH_IAP_UPDATED;
        memset(&info.app_info, 0, sizeof(info.app_info));
    } else if (!app_flash_iap_is_config_valid(&info)) {
        return; /* 配置已损坏，不覆盖 */
    } else if (memcmp(info.net_cfg.ip, ip, 4) == 0 && memcmp(info.net_cfg.mask, mask, 4) == 0 &&
               memcmp(info.net_cfg.gw, gw, 4) == 0 && info.net_cfg.port == port) {
        return; /* 内容未变，跳过擦除 */
    }

    memcpy(info.net_cfg.ip, ip, 4);
    memcpy(info.net_cfg.mask, mask, 4);
    memcpy(info.net_cfg.gw, gw, 4);
    info.net_cfg.port = port;

    app_flash_iap_edit_config(&info);
}

/** @brief 镜像同步：运行态 IP/掩码/网关 + TCP server 端口 → IAP 记录
 *
 *  由 pl_net 的 IP 变更监听触发（任何协议调 pl_net_set_ip 都会走到），
 *  以及 IAP 任务上电对账一次 —— 兜底"本上电周期没有 set_ip 调用"的场景。
 *  update_net_cfg 自带内容去重与空记录骨架，无变化时零擦写。 */
void app_flash_iap_sync_from_runtime(void)
{
    uint8_t ip[4] = {0}, mask[4] = {0}, gw[4] = {0};
    pl_net_get_ip(ip, mask, gw);
    app_flash_iap_update_net_cfg(ip, mask, gw, app_tcp_server_get_port());
}
