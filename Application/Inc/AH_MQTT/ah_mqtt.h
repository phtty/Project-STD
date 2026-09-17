#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "ring_buffer.h"

#include "app_dispatch.h"

#define MQTT_FRAME_MIN_LEN (21U)
#define MQTT_FRAME_MAX_LEN (21U + 512U)

typedef struct [[gnu::packed]] topic_baseinfo {
    char station_hex[8];
    char lane_hex[2];
    char device_type[2];
    char device_id[2];
} topic_info_t;

typedef struct [[gnu::packed]] notify_date {
    char year[4];
    char month[2];
    char day[2];
    char hour[2];
} notify_date_t;

typedef struct [[gnu::packed]] notify_id {
    notify_date_t date_time;
    char device_type[2];
    char device_id[2];
    char send_count[6];
} notify_id_t;

// 设备状态
typedef struct [[gnu::packed]] state_report {
    notify_id_t notify;
    char work_sta;
    char run_sta;
    char reserved[9];
} state_report_t;

// 设备签到
typedef struct [[gnu::packed]] sign_up {
    notify_id_t notify;
    char type;
    char work_sta;
    char soft_ver[10];
    char hard_ver[10];
    char protocol_ver[10];
    char company[10];
    char device[10];
    char reserved[21];
} sign_up_t;

/**
 * @brief AH_MQTT 协议子类
 *
 * base 必须是第一个成员（container_of 偏移 0）。协议自有状态收在这里，不再散落成
 * 文件级全局 —— 探针、任务与命令处理都经 base 取回，多实例也因此成为可能。
 */
typedef struct {
    pcb_t        base;
    topic_info_t topic_info;      /**< 设备标识，用于拼接主题 */
    notify_id_t  notify_id;       /**< 上报帧携带的通知号（对时命令会更新） */
    char         reply_topic[64]; /**< 本次回复的目的主题，随消息交给通道 */
} ah_mqtt_proto_t;

extern osMessageQueueId_t g_proto_ah_matt_queue;
extern osThreadId_t g_ah_mqtt_task_handle;
extern const osThreadAttr_t ProtocolTask_attributes;

void ah_mqtt_handle_task(void *argument);
pcb_probe_sta_t ah_mqtt_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                    uint8_t *scratch, uint16_t scratch_size,
                                    uint32_t *total_len, uint8_t *aux);
