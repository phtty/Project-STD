/**
 * @file    app_dispatch.h
 * @brief   多协议多通道调度框架（Application 层核心）
 *
 * channel_t 使用虚表模式（ch_ops_t）实现 OCP：新增通道类型无需修改此文件。
 * 通道-协议映射由协议模块通过 app_proto_bind_channel 声明。
 * 协议模块通过 sw_app_initcall 自注册，框架零改动。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cmsis_os2.h"
#include "ring_buffer.h"
#include "container_of.h"

/* ---- 常量 ---- */
#define MAX_CHANNELS       (32U)
#define FRAME_DATA_MAX_LEN (1044U)
#define PROTO_MAX_COUNT    (32U)
#define RB_CNT_MAX         (4U)

/* ---- 通道标识 ---- */
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

/* ---- 通道连接状态 ---- */
typedef enum {
    CH_STATE_DOWN = 0,
    CH_STATE_UP   = 1,
} ch_state_t;

/* ---- 帧探测结果 ---- */
typedef enum {
    PROTO_PROBE_READY,
    PROTO_PROBE_WAIT,
    PROTO_PROBE_FAKE,
    PROTO_PROBE_SKIP,
} proto_probe_sta_t;

typedef uint32_t proto_mask_t;

/* ---- 环形缓冲区分组 ---- */
typedef enum {
    RB_GROUP_IAP   = 0,
    RB_GROUP_PROTO = 1,
    RB_GROUP_COUNT = 2,
} rb_group_id_t;

/* ---- 通道抽象（OCP 虚表） ---- */

typedef struct channel channel_t;

typedef struct ch_ops {
    int32_t (*send)(channel_t *ch, const uint8_t *data, uint16_t len);
} ch_ops_t;

typedef struct channel {
    uint8_t ch_id;
    uint8_t state; /* ch_state_t */
    const ch_ops_t *ops;
} channel_t;

/* ---- 调度框架类型 ---- */

typedef proto_probe_sta_t (*proto_probe_fn_t)(const channel_t *ch, const ring_buffer_t *rb, uint32_t *total_len, uint8_t *aux);

typedef struct {
    channel_t *ch;
    uint16_t data_len;
    uint8_t data[FRAME_DATA_MAX_LEN];
} frame_msg_t;

typedef struct {
    void (*init)(void);
} proto_module_entry_t;

typedef struct {
    uint32_t registered_mask;
    ring_buffer_t *proto_rb[PROTO_MAX_COUNT];
    proto_probe_fn_t proto_probe[PROTO_MAX_COUNT];
    osMessageQueueId_t frame_queue[PROTO_MAX_COUNT];
    ring_buffer_t *buf_pool[RB_CNT_MAX];
    osMessageQueueId_t ch_queue;
    proto_mask_t ch_proto_map[CH_ID_MAX];
    channel_t *channels[CH_ID_MAX];
} dispatch_ctx_t;

extern dispatch_ctx_t g_dispatch;

/* ---- 调度 API ---- */
uint8_t proto_index(uint32_t mask);
proto_mask_t app_proto_register(proto_probe_fn_t probe, ring_buffer_t *rb);
void app_proto_set_frame_queue(proto_mask_t mask, osMessageQueueId_t queue);
void app_proto_bind_channel(proto_mask_t mask, channel_id_t ch_id);
ring_buffer_t *app_proto_acquire_buf(uint8_t id, uint16_t size);
void frame_dispatch_task(void *argument);

void app_channel_dispatch(const channel_t *ch, const uint8_t *data, uint16_t len);
void app_channel_register(channel_id_t ch_id, channel_t *ch);
channel_t *app_channel_get(channel_id_t ch_id);
void channel_send(channel_t *ch, uint8_t *data, uint16_t len);
void app_dispatch_init(void);
