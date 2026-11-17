/**
 * @file    app_ah_tcp.h
 * @brief   AH/TCP 参数信道：帧格式常量、0AH/0BH 字段偏移与协议入口
 *
 * 帧格式（《IP 化设备通讯协议》§1.1）：
 *   `FF FF | VER(00) | SEQ | LEN(4, 大端) | DATA | CRC16`
 * CRC = CRC-16/X25，覆盖 VER..DATA（本工程复用 `crc16_xmodem()`）。
 *
 * 字段偏移对 0AH 请求与 0BH 应答**完全一致**（1~83 公共块），
 * 但报文长度/尾部模块必须按命令分别校验。
 */

#pragma once

#include <stdint.h>

#include "app_ah.h"
#include "app_dispatch.h"

#define AH_TCP_STX (0xFFFFU)     /**< 帧起始标志 */
#define AH_TCP_VER (0x00U)       /**< 协议版本 */
#define AH_TCP_CMD_SET (0x0AU)   /**< 参数设置 */
#define AH_TCP_CMD_GET (0x0BU)   /**< 参数采集 */

#define AH_TCP_DATA_MAX (128U)   /**< DATA 域上限（0AH 最长 108、0BH 应答 87） */
#define AH_TCP_FRAME_OVERHEAD (10U) /**< 8 B 帧头 + 2 B CRC */
#define AH_TCP_FRAME_MAX (AH_TCP_DATA_MAX + AH_TCP_FRAME_OVERHEAD) /**< 整帧上限 */
#define AH_TCP_FIXED_LEN (84U)   /**< 0AH 固定部分 / 0BH 应答公共块字节数 */
#define AH_TCP_0A_RSP_LEN (21U)  /**< 0AH 应答字节数 */
#define AH_TCP_0A_OFF_ERR (20U)  /**< 0AH 应答的 ErrCode 偏移 */
#define AH_TCP_0B_REQ_LEN (20U)  /**< 0BH 请求字节数 */
#define AH_TCP_0B_RSP_MAX (AH_TCP_FIXED_LEN + 3U) /**< 0BH 应答上限（含 31H 的 FontLine） */

/* ---- 0AH 请求 / 0BH 应答公共块偏移 ---- */
#define AH_TCP_OFF_CMD (0U)        /**< CmdType */
#define AH_TCP_OFF_UNIX_TM (1U)    /**< UnixTm（4 B，大端） */
#define AH_TCP_OFF_LANE (5U)       /**< LaneHex（5 B） */
#define AH_TCP_OFF_CERT (10U)      /**< CertificationInfo（8 B） */
#define AH_TCP_OFF_RSV (18U)       /**< Reserved（2 B） */
#define AH_TCP_OFF_SERVER1_IP (20U)   /**< ServerIP1 */
#define AH_TCP_OFF_SERVER1_PORT (24U) /**< ServerPort1 */
#define AH_TCP_OFF_SERVER2_IP (26U)   /**< ServerIP2 */
#define AH_TCP_OFF_SERVER2_PORT (30U) /**< ServerPort2 */
#define AH_TCP_OFF_SERVER3_IP (32U)   /**< ServerIP3 */
#define AH_TCP_OFF_SERVER3_PORT (36U) /**< ServerPort3 */
#define AH_TCP_OFF_MQTT_IP (38U)      /**< MQTTIP */
#define AH_TCP_OFF_MQTT_PORT (42U)    /**< MQTTPort */
#define AH_TCP_OFF_MQTT_USER (44U)    /**< MQTTUser（10 B） */
#define AH_TCP_OFF_MQTT_PWD (54U)     /**< MQTTPwd（10 B） */
#define AH_TCP_OFF_MQTT_QOS (64U)     /**< MQTTQOS */
#define AH_TCP_OFF_NTP_IP (65U)       /**< NtpIP */
#define AH_TCP_OFF_CLIENT_IP (69U)    /**< ClientIP */
#define AH_TCP_OFF_CLIENT_PORT (73U)  /**< ClientPort */
#define AH_TCP_OFF_GATEWAY (75U)      /**< Gateway */
#define AH_TCP_OFF_NETMASK (79U)      /**< Netmask */
#define AH_TCP_OFF_MODE_CNT (83U)     /**< ModeCnt */
#define AH_TCP_OFF_MODULES (84U)      /**< 功能模块表起点 */

/** @brief 0AH 请求固定部分 + 模块表 / 0BH 应答公共块 + 模块表的合法长度
 *  @param device_id 本机显示模块类型（31H/32H）
 *  @return 该类型的模块字节数（31H=3、32H=2）；未知类型返回 0 */
static inline uint8_t app_ah_tcp_module_size(uint8_t device_id)
{
    if (device_id == (uint8_t)AH_DEV_FEE_DISPLAY)
        return 3U;
    if (device_id == (uint8_t)AH_DEV_LED_BOARD)
        return 2U;
    return 0U;
}

/** @brief 帧探测（pcb_ops.probe）
 *  @param self 协议控制块
 *  @param ccb 数据来源通道（本协议不使用）
 *  @param src 来源描述（本协议不使用）
 *  @param[out] scratch 框架暂存区；本函数写入窥视到的帧字节
 *  @param scratch_size 暂存区容量
 *  @param[out] total_len 完整帧长度
 *  @param[out] aux 命令码（CmdType）
 *  @return 探测状态，取值见 app_pcb_probe_state_t */
app_pcb_probe_state_t app_ah_tcp_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                             const app_ccb_src_t *src, uint8_t *scratch,
                                             uint16_t scratch_size, uint32_t *total_len,
                                             uint8_t *aux);

/** @brief AH/TCP 协议任务：从帧队列取帧，按 CmdType 分派到 0AH/0BH */
void app_ah_tcp_task(void *argument);
