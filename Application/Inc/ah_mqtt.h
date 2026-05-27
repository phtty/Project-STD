#pragma once

#include "main.h"

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

extern topic_info_t topic_info;
extern notify_date_t notify_date;
extern notify_id_t xNotifyID;

extern osMessageQueueId_t g_proto_ah_matt_queue;
extern osThreadId_t g_ah_mqtt_task_handle;
extern const osThreadAttr_t ProtocolTask_attributes;

void ah_mqtt_handle_task(void *argument);
proto_probe_sta_t ah_mqtt_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *payload_len, uint8_t *cmd_num);
