/**
 * @file    app_iap_cfg.c
 * @brief   IAP 系统配置 Flash 存储（Sector 1, 0x08004000）
 *
 * app_flash_iap_sys_info_t 记录 = magic(4B) | update_status(4B) | FWInfo(40B) | NetConfig(16B) | CRC32(4B)
 * 总长 68B（17 words），按 word 写入 Flash。
 *
 * 操作流程：
 *   init/edit → 填充结构体 → 计算 CRC32 → erase → write
 *   读取 → 直接内存映射 (ADDR_CONFIG_SECTOR) → 空/完整性检查 → 使用
 */

#include "app_iap_cfg.h"
#include "board.h" /* BOARD_HAS_IAP_RECORD */

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

/* 记录扇区**必须落在固件映像之外**。固件起点 = FLASH_BASE + BOARD_VECT_TAB_OFFSET，
   与 boards/&lt;板&gt;/STM32F407XX_FLASH.ld 的 FLASH_ORIGIN 同源（两处由这条断言间接钉住）。

   这两件事是耦合的，只改一个就会回到"擦掉自己"的 HardFault：
     · 给本板加 IAP bootloader → 必须同时
         board.ld 的 FLASH_ORIGIN 改成 0x08040000（前 256KB 留给 bootloader）
         board.h  的 BOARD_VECT_TAB_OFFSET 改成 0x40000
         BOARD_HAS_IAP_RECORD 置 1
     · 只把 BOARD_HAS_IAP_RECORD 置 1 而不动布局 → 这条断言当场编译失败 ✓

   反过来的情况（改了布局忘了置 flag）不会崩，只是记录区白留着不用，属安全失败。 */
#define IAP_FLASH_BASE 0x08000000UL
_Static_assert(BOARD_HAS_IAP_RECORD == 0 ||
                   ADDR_CONFIG_SECTOR < (IAP_FLASH_BASE + BOARD_VECT_TAB_OFFSET),
               /* 断言文本用 ASCII：中文会被 GCC 按八进制转义，在最需要它的时刻读不出来。
                  详细说明在上面的注释里。 */
               "IAP record sector lies inside the firmware image - erasing it erases the "
               "running code. A board WITH a bootloader must move board.ld FLASH_ORIGIN and "
               "BOARD_VECT_TAB_OFFSET together to 0x08040000; a direct-flash board must keep "
               "BOARD_HAS_IAP_RECORD at 0");

/* ---- IAP Flash 存储实例 ---- */
static dev_flash_int_t s_flash_iap = {
    .base        = {.capacity = IAP_SIZE},
    .base_addr = ADDR_CONFIG_SECTOR,
    .sector    = PL_FLASH_SECTOR_1,
};

dev_storage_t *app_flash_iap_get_storage(void)
{
    return &s_flash_iap.base;
}

/* ---- ops 绑定（hw_dev_initcall） ---- */
static void _app_flash_iap_storage_init(void)
{
    s_flash_iap.base.ops = &g_flash_int_ops;
}
hw_dev_initcall(_app_flash_iap_storage_init);

/* ---- 串行化 ----
 * 写这条记录的有两条任务：app_iap_task（队列超时空闲时做镜像对账，
 * app_iap.c 的 s_sync_pending）与 app_ldi_task（0AH）。两边都是
 * "读 → 判定 → 擦扇区 → 编程 17 个 word"。
 *
 * 不加锁的后果不是"某次更新被覆盖"（那种交错最后写者胜，记录仍是完好的），
 * 而是**两个写序列逐 word 交错，拼出一条 CRC 对不上的记录**：
 *
 *     A: 擦除 → program word 0..8
 *     B: 擦除（把 A 刚写的 0..8 抹掉）
 *     B: program word 0..16
 *     A: program word 9..16（覆盖掉 B 的 9..16）
 *
 * 前 9 个 word 来自 B、后 8 个来自 A —— magic 是 B 的、config_crc 是 A 的。
 * 这正是 update_net_cfg 里描述的那种损坏记录。
 *
 * 触发窗口很窄：内部 Flash 擦除期间 CPU 会被停住，两个任务都动不了，要撞上
 * 得正好在编程循环里交错。但后果是"现场需整片重烧"（见下），代价不对等。
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

/* 记录区指针，直接映射到 Flash 地址。
 * 这里刻意暴露成**可写指针**而不是到处用 ADDR_CONFIG_SECTOR 宏：host 单测需要把
 * 记录区重定向到 RAM（0x08004000 在宿主机上不可访问），而重定向只能通过覆盖一个
 * 变量做到，宏做不到。生产代码里两者完全等价，故读取处统一走 g_iap_sys_info。 */
app_flash_iap_sys_info_t *g_iap_sys_info = (app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR;

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

/** @brief 取记录里的网段配置；唯一的合法性判据沿用 app_flash_iap_is_config_valid()，
 *         记录无效时不触碰 out */
bool app_iap_get_net_cfg(app_flash_iap_net_cfg_t *out)
{
    if (!app_flash_iap_is_config_valid(g_iap_sys_info)) return false;

    *out = g_iap_sys_info->net_cfg;
    return true;
}

/* ================================================================
 *  写入操作（先擦除整扇区，再逐 word 编程）
 * ================================================================ */

/* ================================================================
 *  内层原语（调用方须已持 s_lock）
 * ================================================================ */

/* ---- 记录区是否属于本板（BOARD_HAS_IAP_RECORD）----
 * 只有带 IAP bootloader 的板子才有"0x08004000 处的配置记录"这回事：应用从
 * 0x08040000 起，其前的 256KB（Sector 0~5）留给 bootloader，Sector 1 留作记录。
 *
 * **直烧的板子固件从 0x08000000 起铺满整片，而 ADDR_CONFIG_SECTOR 是写死的
 * 0x08004000 —— 那就在固件映像内部。** 对它做任何擦写都是在抹正在执行的代码。
 *
 * 实测（5006048，无 bootloader）：上电约 100ms 后 app_iap.c 的启动对账调
 * update_net_cfg，判定"记录无效"→ 擦 Sector 1 → 擦掉的正是自己的代码 → HardFault。
 * 一旦进这个分支就是每次上电必崩，不是偶发。
 *
 * 所以**写路径整体短路**：擦与写两个最底层原语直接返回错误，上层的一切入口
 * （update_net_cfg / edit_config / erase_config / write_config / init_config）
 * 自然都变成无操作。
 *
 * 读路径留着无害：内存映射读，读到的是固件字节，必然判为无效，调用方回落到默认值。 */
#if !BOARD_HAS_IAP_RECORD
#define IAP_RECORD_ABSENT 1
#else
#define IAP_RECORD_ABSENT 0
#endif

static int32_t _erase_config_unlocked(void)
{
    if (IAP_RECORD_ABSENT) return -1; /* 本板无记录区：绝不能擦 */
    return dev_storage_erase(app_flash_iap_get_storage(), 0, 0);
}

static int32_t _write_config_unlocked(app_flash_iap_sys_info_t *info)
{
    if (IAP_RECORD_ABSENT) return -1; /* 本板无记录区：绝不能写 */
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
    info->update_status = APP_FLASH_IAP_FAILED;
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
 *  **空记录与损坏记录一视同仁，都以有效骨架重建**（magic + update_status 置
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
 *  上位机看到"设置成功"而 IAP 镜像永远没跟上；_iap_cmd_report_ip 读这条记录
 *  会报出全 0xFF。
 *
 *  内容未变时跳过擦除（内部 Flash 擦除会硬停总线）。注意"损坏"分支不走这条
 *  去重 —— 记录本来就坏，必须重写才算修好。 */
void app_flash_iap_update_net_cfg(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                  uint32_t port)
{
    /* 本板没有记录区：直接返回，连"是否损坏"都不判 —— 那块地址上是固件自身，
       按"记录"去读必然判为无效，会打出一条误导性的"记录损坏"日志。 */
    if (IAP_RECORD_ABSENT) return;

    app_flash_iap_sys_info_t info;

    if (s_lock) osMutexAcquire(s_lock, osWaitForever);

    memcpy(&info, (void *)g_iap_sys_info, sizeof(info));

    bool empty = app_flash_iap_is_config_empty(&info);
    bool valid = !empty && app_flash_iap_is_config_valid(&info);

    if (!valid) {
        if (!empty)
            printf("[iap_cfg] IAP 记录损坏（magic=0x%08X），按空记录以有效骨架重建\n",
                   (unsigned)info.magic);
        info.magic      = APP_FLASH_IAP_MAGIC;
        info.update_status = APP_FLASH_IAP_UPDATED;
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
