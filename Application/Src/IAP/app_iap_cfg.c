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

#include <stdio.h> /* printf —— 损坏记录重建时要留痕 */
#include <string.h>
#include "cmsis_os2.h"
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

/* ---- 串行化 ----
 * 写这条记录的有两条任务：iap_handle_task（队列超时空闲时做镜像对账，
 * app_iap.c 的 s_sync_pending）与 ldi_handle_task（0AH）。两边都是
 * "读记录 → 判定 → 擦扇区 → 编程"，交错就会丢更新：
 *
 *     LDI 任务：读记录（app_info = A）
 *     IAP 任务：写记录（app_info = B）
 *     LDI 任务：写回（app_info = A）      ← B 的更新没了
 *
 * 窗口不大（0AH 是手工操作），但真实存在，且丢的是固件版本信息这类
 * 平时没人看、出事才用的字段。锁把整个读-改-擦-写包住。
 *
 * 在 sw_dev(2) 创建：早于任何协议任务，故下面的入口里可以直接用。
 * if (s_lock) 判空是为了兼容"锁未建好就被调用"（如 hw initcall 阶段）。 */
static osMutexId_t s_lock;

static void _iap_cfg_lock_init(void)
{
    const osMutexAttr_t attr = {.name = "iap_cfg", .attr_bits = osMutexPrioInherit};
    s_lock                   = osMutexNew(&attr);
}
sw_dev_initcall(_iap_cfg_lock_init);

app_flash_iap_sys_info_t *g_config = (app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR; /* 直接映射到 Flash 地址 */

/* ---- CRC32 覆盖范围 ----
 * 覆盖整条记录**除去 config_crc 字段自身**。这个公式原先在本文件里写了三遍
 * （校验、init、edit 各一份），结构体一变就得三处同时改对，改漏一处不会报错、
 * 只会让记录永远校验不过（而"校验不过"又会被当成损坏去重建，症状更绕）。
 * 收成一处。 */
static uint32_t _iap_cfg_crc(const app_flash_iap_sys_info_t *info)
{
    return pl_crc32_calc(pl_crc_get_handle(), (const uint8_t *)info,
                         sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc));
}

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

    return info->config_crc == _iap_cfg_crc((const app_flash_iap_sys_info_t *)info);
}

/* ================================================================
 *  写入操作（先擦除整扇区，再逐 word 编程）
 * ================================================================ */

/* ================================================================
 *  内层原语（调用方须已持 s_lock）
 * ================================================================ */

static int32_t _erase_config_unlocked(void)
{
    return dev_storage_erase(app_flash_iap_get_storage(), 0, 0);
}

static int32_t _write_config_unlocked(app_flash_iap_sys_info_t *info)
{
    return dev_storage_write(app_flash_iap_get_storage(), 0, (uint8_t *)info, sizeof(*info));
}

static void _edit_config_unlocked(app_flash_iap_sys_info_t *info)
{
    info->config_crc = _iap_cfg_crc(info);
    _erase_config_unlocked();
    _write_config_unlocked(info);
}

/* ================================================================
 *  公开入口（各自持锁）
 * ================================================================ */

/** @brief 初始化配置：设置 magic + 默认 IP(192.168.114.200:9529) + 空固件信息，写入 Flash */
void app_flash_iap_init_config(app_flash_iap_sys_info_t *info)
{
    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

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

    /* 不必在这里算 CRC：_edit_config_unlocked 会算好再写 */
    _edit_config_unlocked(info);

    if (s_lock) osMutexRelease(s_lock);
}

/** @brief 修改配置：更新 CRC32 → 擦除 → 写入 */
void app_flash_iap_edit_config(app_flash_iap_sys_info_t *info)
{
    if (s_lock) osMutexAcquire(s_lock, osWaitForever);
    _edit_config_unlocked(info);
    if (s_lock) osMutexRelease(s_lock);
}

/** @brief 擦除 Sector 1（0x08004000，16KB） */
int32_t app_flash_iap_erase_config(void)
{
    if (s_lock) osMutexAcquire(s_lock, osWaitForever);
    int32_t r = _erase_config_unlocked();
    if (s_lock) osMutexRelease(s_lock);
    return r;
}

/** @brief 写入 app_flash_iap_sys_info_t 到 Flash */
int32_t app_flash_iap_write_config(app_flash_iap_sys_info_t *info)
{
    if (s_lock) osMutexAcquire(s_lock, osWaitForever);
    int32_t r = _write_config_unlocked(info);
    if (s_lock) osMutexRelease(s_lock);
    return r;
}

/** @brief 同步设备网络配置到内部 Flash（读-改-net_cfg-写，不碰其他字段）
 *
 *  **空记录与损坏记录一视同仁，都以有效骨架重建**（magic + update_sta 置
 *  UPDATED，app_info 清零，net_cfg 随后覆盖）。
 *
 *  空记录要重建是旧版就有的（否则写出来的是 magic 仍为 0xFFFFFFFF 的永久
 *  无效记录）；损坏记录原先写的是"拒绝覆盖"，那是个更糟的选择 ——
 *  `is_config_valid` 失败后这条记录**永远不会再被碰**，而
 *  `app_flash_iap_init_config()` 全工程零调用者，等于没有任何修复路径。
 *
 *  损坏态是可达的：`_edit_config_unlocked` 是"先擦除、再按 word 0→16 顺序
 *  编程"，magic 是第 0 个 word，config_crc 是**最后一个**。编程中途掉电即得
 *  magic 已写、crc 仍是 0xFFFFFFFF 的记录 —— is_config_empty() 假、
 *  is_config_valid() 假，卡死在这个状态。此时唯一的修复手段是整片擦除重烧。
 *
 *  并且它的表现很隐蔽：0AH 的保存状态取自 LDI 自己那条记录（是成功的），
 *  上位机看到"设置成功"而 IAP 镜像永远没跟上；cmd_ReportIp_01 读这条记录
 *  会报出全 0xFF。
 *
 *  内容未变时跳过擦除（内部 Flash 擦除会硬停总线）。注意"损坏"分支不走这条
 *  去重 —— 记录本来就坏，必须重写才算修好。 */
void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    app_flash_iap_sys_info_t info;

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    memcpy(&info, (void *)ADDR_CONFIG_SECTOR, sizeof(info));

    bool empty = app_flash_iap_is_config_empty(&info);
    bool valid = !empty && app_flash_iap_is_config_valid(&info);

    if (!valid) {
        if (!empty)
            printf("[iap_cfg] IAP 记录损坏（magic=0x%08X），按空记录以有效骨架重建\n",
                   (unsigned)info.magic);
        info.magic      = APP_FLASH_IAP_MAGIC;
        info.update_sta = APP_FLASH_IAP_UPDATED;
        memset(&info.app_info, 0, sizeof(info.app_info));
    } else if (memcmp(info.net_cfg.ip, ip, 4) == 0 && memcmp(info.net_cfg.mask, mask, 4) == 0 &&
               memcmp(info.net_cfg.gw, gw, 4) == 0 && info.net_cfg.port == port) {
        if (s_lock) osMutexRelease(s_lock);
        return; /* 内容未变，跳过擦除 */
    }

    memcpy(info.net_cfg.ip, ip, 4);
    memcpy(info.net_cfg.mask, mask, 4);
    memcpy(info.net_cfg.gw, gw, 4);
    info.net_cfg.port = port;

    _edit_config_unlocked(&info);

    if (s_lock) osMutexRelease(s_lock);
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
