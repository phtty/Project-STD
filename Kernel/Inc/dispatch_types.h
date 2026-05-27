/**
 * @file    dispatch_types.h
 * @brief   调度框架共享类型（Device ↔ Application 共用）
 *
 * 仅包含被 Device 层和 Application 层共同引用的类型定义。
 * channel_t 使用虚表模式（ch_ops_t）实现 OCP：新增通道类型无需修改此文件。
 * 通道-协议映射由 Application 层在 channel_proto_map[] 中统一维护，
 * Device 层只持有 ch_id 标识自己，不感知具体协议。
 *
 * 协议相关的类型（proto_mask_t、frame_msg_t 等）归 Application 层定义。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ring_buffer.h"

#define MAX_CHANNELS       (32U)
#define FRAME_DATA_MAX_LEN (1044U)

/** @brief 通道标识（Device 层定义，Application 层用于映射协议） */
typedef enum {
    CH_ID_RS485      = 0,
    CH_ID_RS232      = 1,
    CH_ID_TCP_SERVER = 2,
    CH_ID_TCP_CLIENT = 3,
    CH_ID_UDP        = 4,
    CH_ID_MQTT       = 5,
    CH_ID_RS232_1    = 6, /**< Project_STD 独有: 第二路 RS232 (USART6) */
    CH_ID_MAX        = 7,
} channel_id_t;

/** @brief 环形缓冲区分组 */
typedef enum {
    RB_GROUP_IAP = 0,
    RB_GROUP_PROTO,
    RB_GROUP_COUNT,
} rb_group_id_t;

/** @brief 帧探测结果 */
typedef enum {
    PROTO_PROBE_READY, /**< 完整帧就绪，可推入协议队列处理 */
    PROTO_PROBE_WAIT,  /**< 数据不足，等待更多字节 */
    PROTO_PROBE_FAKE,  /**< 伪帧头，跳过 1 字节继续探测 */
    PROTO_PROBE_SKIP,  /**< 帧结构合法但不属于本设备，跳过 total_len 字节 */
} proto_probe_sta_t;

/** @brief container_of — 从成员指针找回包含它的结构体 */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ---- 前向声明（ch_ops_t 引用 channel_t *，但 channel_t 定义在后） ---- */
typedef struct channel channel_t;

/** @brief 通道操作虚表（OCP：新增通道类型只需提供 send 实现） */
typedef struct ch_ops {
    int32_t (*send)(channel_t *ch, const uint8_t *data, uint16_t len);
} ch_ops_t;

/** @brief 通道连接状态 */
typedef enum {
    CH_STATE_DOWN = 0, /**< 未连接 / 已断开 */
    CH_STATE_UP   = 1, /**< 已连接 / 就绪 */
} ch_state_t;

/**
 * @brief 通道父类（所有通道子类必须将其作为第一个成员）
 *
 * 仅包含所有通道共有的标识、状态和虚表。私有数据（UART、netconn、topic 等）
 * 存放在子类中，通过 container_of 访问。
 */
typedef struct channel {
    uint8_t ch_id;
    uint8_t state; /**< ch_state_t，通道 init/deinit 时维护，供上层查询连接状态 */
    const ch_ops_t *ops;
} channel_t;

/* ---- 调度 API（Kernel 声明，Application 实现） ---- */
void app_channel_dispatch(const channel_t *ch, const uint8_t *data, uint16_t len);
void app_channel_register(channel_id_t ch_id, channel_t *ch);
channel_t *app_channel_get(channel_id_t ch_id);
void channel_send(channel_t *ch, uint8_t *data, uint16_t len);
void app_dispatch_init(void);
