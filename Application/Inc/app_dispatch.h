/**
 * @file    app_dispatch.h
 * @brief   多协议多通道调度框架（Application 层核心）
 *
 * channel_t 使用虚表模式（ch_ops_t）实现 OCP：新增通道类型无需修改此文件。
 * 通道-协议映射由协议模块通过 app_proto_bind_channel 声明。
 * 协议模块通过 sw_app_initcall 自注册，框架零改动。
 *
 * RB：一物理通道一环缓（RJ45 / RS485 / RS232）；体由协议 TU 以 RB_PROVIDE_WEAK
 * 编译期提供，未编入则槽为 NULL。RS232_1（USART6）旁路语音 TX，禁止 bind。
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

/* ---- 物理通道 RB 槽（与 CH_ID 解耦；网口逻辑通道共享 RJ45 槽）---- */
typedef enum {
    RB_SLOT_RJ45  = 0, /**< ETH：IAP/TCP/UDP/MQTT/地区协议共享 */
    RB_SLOT_RS485 = 1, /**< USART1：地区协议链式 */
    RB_SLOT_RS232 = 2, /**< USART3：地区协议链式 */
    RB_SLOT_COUNT = 3,
} rb_slot_t;

#define RB_CNT_MAX ((uint8_t)RB_SLOT_COUNT)

#define RB_SIZE_RJ45  (1536U) /* ≥ IAP 最大帧 1044 + 余量；原 2304 瘦身 */
#define RB_SIZE_RS485 (768U)  /* 原 512 +256，覆盖 RLS 满屏位图 ≤530 */
#define RB_SIZE_RS232 (768U)  /* 与 RS485 对齐；青海最大帧 259 */

/* 协议 TU 的 RB_PROVIDE_WEAK 入口名（与 app_dispatch.c 一致） */
#define RB_PROVIDE_RJ45  rb_provide_rj45
#define RB_PROVIDE_RS485 rb_provide_rs485
#define RB_PROVIDE_RS232 rb_provide_rs232

/* ---- 通道标识 ---- */
typedef enum {
    CH_ID_RS485      = 0,
    CH_ID_RS232      = 1,
    CH_ID_TCP_SERVER = 2,
    CH_ID_TCP_CLIENT = 3,
    CH_ID_UDP        = 4,
    CH_ID_MQTT       = 5,
    CH_ID_RS232_1    = 6, /**< [仅语音 TX] USART6 — 禁止任何协议 bind_channel */
    CH_ID_UDP_CQ     = 7, /**< CQ 业务口 UDP（PROTO_CHONGQING 读 Sector1 net_cfg.port 默认 20103；dev 构建固定 20103） */
    CH_ID_MAX        = 8,
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
    uint8_t data[]; /* 柔性数组, sizeof(frame_msg_t)=8 */
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
extern osThreadId_t g_dispatch_task_handle;

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
int32_t channel_send(channel_t *ch, uint8_t *data, uint16_t len);
void app_dispatch_init(void);
