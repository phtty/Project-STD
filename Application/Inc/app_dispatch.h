/**
 * @file    app_dispatch.h
 * @brief   协议调度框架（Application 层核心）
 *
 * 协议模块通过 APP_PROTO_MODULE() 自注册（linker section），
 * app_dispatch_init() 自动遍历所有注册模块并创建任务。
 * 新增协议只需写新的协议模块文件，框架零改动。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cmsis_os2.h"
#include "dispatch_types.h"

/** @brief 最大协议数量（bitmask 为 32 位） */
#define PROTO_MAX_COUNT (32U)

/** @brief 环形缓冲区池容量（按需调整） */
#define RB_CNT_MAX (4U)

/** @brief 协议掩码 */
typedef enum {
    PROTO_MASK_NONE = (uint32_t)0b0000,
    PROTO_MASK_IAP     = (uint32_t)0b0001,
    PROTO_MASK_LDI     = (uint32_t)0b0010,
    PROTO_MASK_AH_MQTT = (uint32_t)0b0100,
    PROTO_MASK_ALL     = (uint32_t)0xffffffff,
} proto_mask_t;

/** @brief 协议探测函数类型 */
typedef proto_probe_sta_t (*proto_probe_fn_t)(const channel_t *ch, const ring_buffer_t *rb,
                                              uint32_t *total_len, uint8_t *aux);

/** @brief 帧消息（调度任务 → 协议处理任务） */
typedef struct {
    channel_t *ch;
    uint16_t data_len;
    uint8_t data[FRAME_DATA_MAX_LEN];
} frame_msg_t;

/** @brief 协议模块自注册条目（存放于 .sw_initcall linker 段，通过 sw_device_initcall 宏使用） */
typedef struct {
    void (*init)(void); /**< 协议模块初始化函数（创建任务 + 注册协议 + 绑定通道） */
} proto_module_entry_t;

/**
 * @brief 调度上下文——协议调度框架的全部运行时状态
 *
 * 所有调度函数统一通过 g_dispatch 访问此结构体，
 * 新增调度相关字段只需在此处增加即可。
 */
typedef struct {
    /* ---- 协议注册表 ---- */
    uint8_t proto_count;                             /**< 实际已注册的协议数量（app_proto_register 内部递增） */
    ring_buffer_t *proto_rb[PROTO_MAX_COUNT];        /**< 协议 → 环形缓冲区指针（app_proto_register 设置） */
    proto_probe_fn_t proto_probe[PROTO_MAX_COUNT];   /**< 协议 → 帧探测函数（app_proto_register 设置） */
    osMessageQueueId_t frame_queue[PROTO_MAX_COUNT]; /**< 协议 → 帧消息队列句柄（协议任务创建并设置） */

    /* ---- 环形缓冲区池 ---- */
    ring_buffer_t *buf_pool[RB_CNT_MAX]; /**< id → 已初始化的 ring buffer 指针（app_proto_acquire_buf 懒初始化） */

    /* ---- 调度队列 ---- */
    osMessageQueueId_t ch_queue; /**< 通道指针通知队列句柄（每项 sizeof(channel_t *)，frame_dispatch_task 阻塞等待） */

    /* ---- 通道-协议映射 ---- */
    proto_mask_t ch_proto_map[CH_ID_MAX]; /**< 通道 → 协议掩码位图（app_proto_bind_channel 逐位 OR 累积，ch_id 为索引） */

    /* ---- 通道注册表 ---- */
    channel_t *channels[CH_ID_MAX]; /**< ch_id → 通道指针（通道 init/deinit 时调用 app_channel_register 维护） */
} dispatch_ctx_t;

extern dispatch_ctx_t g_dispatch;

/* ---- 调度 API ---- */
uint8_t proto_index(uint32_t mask);
void app_proto_register(uint32_t mask, proto_probe_fn_t probe, ring_buffer_t *rb);
void app_proto_set_frame_queue(uint32_t mask, osMessageQueueId_t queue);
void app_proto_bind_channel(proto_mask_t mask, channel_id_t ch_id);
ring_buffer_t *app_proto_acquire_buf(uint8_t id, uint16_t size);
void frame_dispatch_task(void *argument);
void app_channel_register(channel_id_t ch_id, channel_t *ch);
channel_t *app_channel_get(channel_id_t ch_id);
