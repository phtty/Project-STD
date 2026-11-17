/**
 * @file    app_ah_cfg.h
 * @brief   AH 参数配置：0AH 线格式载荷、默认值、校验与 ah_cfg 记录读写
 *
 * 载荷与《IP 化设备通讯协议》§2.1 的 1~83 字段一一对应（去掉瞬态的 CmdType/UnixTm/Reserved）。
 * 字段值按**主机字节序**存放；0AH/0BH 的组帧/解析由 AH/TCP 模块显式做大端转换。
 *
 * 记录名 `"ah_cfg"` 与版本号是**持久化契约**：改名/改布局会让旧记录判 INVALID 并回落默认值。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ah.h"

#define APP_AH_CFG_RECORD_NAME "ah_cfg" /**< W25Qxx 配置记录名（契约） */
#define APP_AH_CFG_VERSION (1U)         /**< 记录格式版本（布局变更时 +1） */
#define APP_AH_MQTT_CRED_LEN (10U)      /**< MQTT 用户名/密码定长（0x00 填充） */
#define APP_AH_CFG_SIZE (80U)           /**< 载荷字节数（契约） */
#define APP_AH_FONT_LINE_MAX (6U)       /**< 31H 默认显示行数上限 */

/**
 * @brief AH 参数（0AH 的持久化部分）
 *
 * 约定：
 *   - 未配置/不使用的 IP、端口、字符串一律为 0，消费者按“0 = 不启用”处理；
 *   - `mqtt_user/mqtt_pwd` 是定长 10 字节，**可能没有 NUL**，用 `memcpy` + 长度处理；
 *   - `device_id` 必须是本板 `BOARD_AH_DISPLAY_TYPE`（加载时强制校正）。
 */
struct [[gnu::packed]] app_ah_cfg {
    uint8_t lane_hex[5];              /**< 车道 HEX：站 4 B + 车道 1 B */
    uint8_t cert[8];                  /**< CertificationInfo（身份码，只存不校验） */
    uint8_t server1_ip[4];            /**< 外场控制服务 IP（本期只存） */
    uint16_t server1_port;            /**< 外场控制服务端口 */
    uint8_t server2_ip[4];            /**< 升级服务 IP（本期只存） */
    uint16_t server2_port;            /**< 升级服务端口 */
    uint8_t server3_ip[4];            /**< 预留服务 IP */
    uint16_t server3_port;            /**< 预留服务端口 */
    uint8_t mqtt_ip[4];               /**< MQTT Broker IP */
    uint16_t mqtt_port;               /**< MQTT Broker 端口 */
    char mqtt_user[APP_AH_MQTT_CRED_LEN]; /**< MQTT 用户名（定长 0x00 填充） */
    char mqtt_pwd[APP_AH_MQTT_CRED_LEN];  /**< MQTT 密码（定长 0x00 填充） */
    uint8_t mqtt_qos;                 /**< MQTT QOS（文档标“预留”，只存不用） */
    uint8_t ntp_ip[4];                /**< 对时服务器 IP（本期只存） */
    uint8_t client_ip[4];             /**< 设备 IP */
    uint16_t client_port;             /**< 设备端口 = AH/TCP 参数服务监听口 */
    uint8_t gateway[4];               /**< 设备网关 */
    uint8_t netmask[4];               /**< 设备子网掩码 */
    uint8_t device_id;                /**< 显示模块类型，= BOARD_AH_DISPLAY_TYPE */
    uint8_t device_no;                /**< 显示模块序号（主题第 4 段） */
    uint8_t font_line;                /**< 31H 默认行数 0~6；32H 恒 0 */
    uint8_t reserved[1];              /**< 版本演进预留 */
};

static_assert(sizeof(app_ah_cfg_t) == APP_AH_CFG_SIZE, "AH 配置载荷必须为 80 字节");

/** @brief 填出厂默认值（网络地址优先取 IAP 记录镜像，其次当前 netif）
 *  @param[out] out 输出配置；传 nullptr 时本函数直接返回
 *  @note 默认值不保证通过 `app_ah_cfg_validate()`：首次上电可能是 0.0.0.0，
 *        等 0AH 下发或 LDI 配好网络后再变合法。 */
void app_ah_cfg_defaults(app_ah_cfg_t *out);

/** @brief 取当前配置（必要时触发一次记录加载；失败/空记录回落默认值）
 *  @param[out] out 输出配置；传 nullptr 时本函数直接返回
 *  @note 返回的永远是“可用的配置”：记录无效时是默认值，不是错误。 */
void app_ah_cfg_get(app_ah_cfg_t *out);

/** @brief 保存配置到 `"ah_cfg"` 记录，成功后同步 RAM 镜像
 *  @param cfg 待保存配置（只读；先过 `app_ah_cfg_validate()`）
 *  @return 0 成功（含内容未变而去重跳过）；负值失败（参数非法/未注册/Flash 错误）
 *  @note 失败时不改 RAM：避免“看着生效、重启变回去”。 */
int32_t app_ah_cfg_save(const app_ah_cfg_t *cfg);

/** @brief 校验配置是否可写入
 *  @param cfg 待校验配置（只读）
 *  @param[out] err 失败时写入错误码；可为 nullptr
 *  @return true 合法；false 非法（`err` 为 `APP_AH_ERR_PARAM`）
 *  @note 0 值按“未配置”放行（`client_port` 除外，参数服务必须有端口）。 */
bool app_ah_cfg_validate(const app_ah_cfg_t *cfg, app_ah_err_t *err);
