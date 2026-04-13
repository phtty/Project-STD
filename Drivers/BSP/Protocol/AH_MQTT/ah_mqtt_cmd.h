#pragma once

#include "main.h"

#include "ah_mqtt.h"

// 显示控制指令
typedef struct [[gnu::packed]] cmd_disp {
    notify_id_t nid;
    char scrn_sw;
    char color;
    char size;
    char text[];
} cmd_display_t;

// 屏幕填充
typedef struct [[gnu::packed]] cmd_fill {
    notify_id_t nid;
    char color;
} cmd_fill_t;

// 重启
typedef struct [[gnu::packed]] cmd_restart {
    notify_id_t nid;
    char type;
    char set;
    char time[14];
    char reserve;
} cmd_restart_t;

// 对时
typedef struct [[gnu::packed]] cmd_checktime {
    notify_id_t nid;
    char type;
    char time[14];
} cmd_checktime_t;

typedef void (*ah_mqtt_cmd_handler_fn_t)(ch_meta_t *, char *);
extern const ah_mqtt_cmd_handler_fn_t g_ah_mqtt_cmd_table[];
