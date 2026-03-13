#pragma once

#include "main.h"

#include "cmsis_os2.h"
#include "RingBuff.h"

#define FRAME_MIN_LEN (21U)
#define FRAME_MAX_LEN (21U + 512U)

typedef struct topic_baseinfo {
    char station_hex[8];
    char lane_hex[2];
    char device_type[2];
    char device_id[2];
} __attribute__((packed)) topic_info_t;

typedef struct notify_date {
    char year[4];
    char month[2];
    char day[2];
    char hour[2];
} __attribute__((packed)) notify_date_t;

typedef struct notify_id {
    notify_date_t date_time;
    char device_type[2];
    char device_id[2];
    char send_count[6];
} __attribute__((packed)) notify_id_t;

// 设备状态
typedef struct state_report {
    notify_id_t notify;
    char work_sta;
    char run_sta;
    char reserved[9];
} __attribute__((packed)) state_report_t;

// 设备签到
typedef struct sign_up {
    notify_id_t notify;
    char type;
    char work_sta;
    char soft_ver[10];
    char hard_ver[10];
    char protocol_ver[10];
    char company[10];
    char device[10];
    char reserved[21];
} __attribute__((packed)) sign_up_t;

extern topic_info_t topic_info;
extern notify_date_t notify_date;
extern notify_id_t xNotifyID;

extern osThreadId_t ProtocolHandle;
extern const osThreadAttr_t ProtocolTask_attributes;

void ProtocolTask(void *argument);
void handle_topic(const char topic[]);
