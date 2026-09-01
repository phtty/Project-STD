/**
 * @file    app_board_net_cfg.h
 * @brief   板级系统配置存储（内部 Flash Sector 1, 0x08004000, 16KB）
 *
 * 本模块是 Sector1 配置记录的**唯一布局归属**，LDI 与 IAP 协议共同依赖：
 *  - 网络配置（ip/mask/gw 共享 + port/udp_port 双端口）：LDI 0AH / IAP 4B02 写
 *    port（TCP 口），CQ setip 写 udp_port（UDP 口）；Bootloader / Recovery / 主固件
 *    共享同一套上电 IP；
 *  - 固件信息 + 升级状态（app_info / update_sta）：IAP 升级流程专用，与 net_cfg
 *    同扇区同记录，由本模块统一做读-改-写（16KB 整扇区擦除），避免双模块擦写
 *    所有权冲突。
 *
 * 记录布局与 Bootloader / Recovery 各自的 Drivers/BSP/Config/config_info.h 二进制
 * 兼容，由下方 static_assert 锁定尺寸与偏移。写入守卫语义（空/损坏自愈、升级中间态
 * 仅放行 net_cfg 字段更新、同值跳过擦写）见 doc/07_LDI与IAP配置解耦。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* ---- 地址与常量定义（Sector1 配置区，仅本模块持有） ---- */
#define ADDR_CONFIG_SECTOR 0x08004000

#define APP_BOARD_NET_CFG_MAGIC 0x0d000721

/* ---- Sector1 记录布局 ---- */

/* 网络配置
 * 字段语义（方案 B，2026-08-21，TCP/UDP 端口彻底分离）：
 *  - port     = TCP 业务口（LDI 0AH 写 / IAP 0x02 写 / 0x01 报 / TCP Server 监听，
 *               LDI/IAP 生态，出厂默认 9528）；
 *  - udp_port = CQ UDP 业务口（CQ setip 写 / CQ UDP 通道读，出厂默认 20103）。
 * ip/mask/gw 两套口共享（setip 与 0AH 都写，现状不变）。 */
__attribute__((aligned(4))) typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint32_t port;
    uint32_t udp_port;
} app_board_net_cfg_t;

/* main app 信息 */
__attribute__((aligned(4))) typedef struct {
    uint32_t size;  /* Main App 字节长度 */
    uint32_t crc32; /* Main App CRC32 值 */
    char version[32];
} app_board_fw_info_t;

/* 升级状态机 */
typedef enum {
    APP_BOARD_UPDATED  = 0,
    APP_BOARD_UPDATING = 1,
    APP_BOARD_FAILED   = 2,
} app_board_update_sta_t;

/* 存储在 0x08004000 */
__attribute__((aligned(4))) typedef struct {
    uint32_t magic;                   /* 魔数，判断配置区是否有效 */
    uint32_t update_sta;              /* 升级状态机 */
    app_board_fw_info_t app_info;     /* main_app 状态 */
    app_board_net_cfg_t net_cfg;      /* 网络配置 */
    uint32_t config_crc;              /* 本结构体自身的 CRC32 校验 */
} app_board_sys_info_t;

/* ---- 布局锁定：与 Bootloader / Recovery config_info.h 二进制兼容 ---- */
static_assert(sizeof(app_board_net_cfg_t) == 20, "net_cfg 布局 20B");
static_assert(sizeof(app_board_fw_info_t) == 40, "fw_info 布局 40B");
static_assert(sizeof(app_board_sys_info_t) == 72, "sys_info 布局 72B");
static_assert(offsetof(app_board_sys_info_t, magic) == 0, "magic 偏移 0");
static_assert(offsetof(app_board_sys_info_t, update_sta) == 4, "update_sta 偏移 4");
static_assert(offsetof(app_board_sys_info_t, app_info) == 8, "app_info 偏移 8");
static_assert(offsetof(app_board_sys_info_t, net_cfg) == 48, "net_cfg 偏移 48");
static_assert(offsetof(app_board_net_cfg_t, port) == 12, "port 偏移 12");
static_assert(offsetof(app_board_net_cfg_t, udp_port) == 16, "udp_port 偏移 16");
static_assert(offsetof(app_board_sys_info_t, config_crc) == 68, "config_crc 偏移 68");

/**
 * @brief 读当前 Sector1 整记录（内存映射拷贝，无有效性判定）
 *
 * 供 IAP 命令读 app_info / update_sta 等非网络字段；网络配置读取请用
 * app_board_net_cfg_get。记录无效时读出内容为擦除态或损坏残留，调用方自行把关。
 */
void app_board_net_cfg_read(app_board_sys_info_t *out);

/**
 * @brief 读网络配置（记录有效时）
 *
 * @return 0 成功且 out 已填充；-1 记录无效（magic 不匹配或 CRC 错）
 */
int32_t app_board_net_cfg_get(app_board_net_cfg_t *out);

/**
 * @brief 同步设备 IP/掩码/网关/TCP 口/UDP 口 到内部 Flash（LDI 0AH / IAP 4B02 / CQ setip 时调用）
 *
 * 字段语义（方案 B，2026-08-21）：port = TCP 业务口（LDI/IAP 生态），udp_port = CQ UDP
 * 业务口。调用方按协议口径填对应端口、另一端口保留现值（先 get，失败用出厂默认）——
 * LDI 0AH / IAP 4B02 写 port、保留 udp_port；CQ setip 写 udp_port、保留 port。
 *
 * 分支语义（见 doc/07_LDI与IAP配置解耦 02 §9/§10）：
 *  - 空扇区：完整初始化，镜像老 Bootloader Init_Config_Info 首次上电语义
 *    （magic + update_sta=updated + app_info 全 0 且 crc32=0xFFFFFFFF + net_cfg + CRC32）；
 *  - 损坏记录（magic 无效或 CRC 错）：按空扇区处理，完整初始化后写新配置（自愈）；
 *  - 有效记录且 update_sta != updated（升级中间态）：仅更新 net_cfg，update_sta 与
 *    app_info 原样保留（Bootloader 条件 D 判定不受影响）（2026-08-18 修订）；
 *  - 有效记录且 update_sta == updated：仅更新 net_cfg（ip/mask/gw/port/udp_port），其余
 *    字段原样保留；与现扇区 net_cfg 完全一致（含 port 与 udp_port）时跳过擦写。
 *
 * @return 0 成功（含同值跳过）；<0 为擦/写错误码
 */
int32_t app_board_net_cfg_update(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                 uint32_t port, uint32_t udp_port);

/**
 * @brief 同步固件版本号到内部 Flash（app_info.version[32]，IAP 0x03「上报固件状态」用）
 *
 * 只允许改 app_info.version 并重算 config_crc，**绝不动** app_info.size / app_info.crc32 /
 * net_cfg / update_sta 的既有值（size/crc32 由 Recovery 升级流程写入，改动会触发 Bootloader
 * 条件 D 判 App 损坏）。
 *
 * 分支语义（与 app_board_net_cfg_update 同构，见 doc/07_LDI与IAP配置解耦 02 §13）：
 *  - 空扇区：完整初始化（magic + update_sta=updated + app_info 全 0 且 crc32=0xFFFFFFFF +
 *    net_cfg 全 0）+ 填 version。调用点安排在 sw_board_init()（含 ldi_ctx_init 从 W25Qxx
 *    回写 net_cfg）之后，正常路径不会走到此分支；若走到（如 ldi_ctx_init 未回写），
 *    net_cfg 会被写成 0.0.0.0/port0，由后续 0AH / 上电同步恢复；
 *  - 损坏记录（magic 无效或 CRC 错）：按空扇区处理（自愈），完整初始化后填 version；
 *  - 有效记录且 update_sta != updated（升级中间态，Bootloader 条件 A/B 写入）：拒绝覆盖，
 *    返回 -1；
 *  - 有效记录且 update_sta == updated：仅更新 app_info.version，其余字段原样保留；与现扇区
 *    version 一致时跳过擦写（Q3：不做磨损均衡）。
 *
 * @param version 版本字符串（如 PROGRAM_CODE），须满足 strlen(version) < 32
 * @return 0 成功（含同值跳过）；<0 失败（-1 = 升级中间态拒绝覆盖，-2 = version 过长，
 *         其余为擦/写错误码）
 */
int32_t app_board_net_cfg_fw_version_update(const char *version);