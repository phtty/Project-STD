#pragma once

#include <stdint.h>

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

/**
 * 命令处理函数：与探针一致地回传协议对象，派生实现用 container_of 取回自身状态。
 * 命令分类由探针在投递当下给出并经 frame_msg_t.aux 传来，处理函数不必再解析来源主题。
 */
typedef void (*ah_mqtt_cmd_handler_fn_t)(pcb_t *self, ccb_t *ccb, char *data);

/**
 * 一条命令 = 订阅主题 + 处理函数。
 *
 * 主题与处理函数放在同一张表里，**索引即命令号**：通道按它订阅、探针按它分类、
 * 任务按它分派。此前主题在通道与协议里各写一份（订阅 "ASK/x" 却按 "/ASK/x" 分类），
 * 改一条命令要动两处、不一致时还会静默失配；合并成一张表后不可能再漂移。
 */
typedef struct {
    const char               *topic; /**< 订阅主题；由通道负责订阅 */
    ah_mqtt_cmd_handler_fn_t  handler;
} ah_mqtt_cmd_entry_t;

#define AH_MQTT_CMD_COUNT (4U)

extern const ah_mqtt_cmd_entry_t g_ah_mqtt_cmd_table[AH_MQTT_CMD_COUNT];
