#pragma once

/**
 * @file    app_ahmq_cmd.h
 * @brief   AHMQ 命令定义：各命令载荷结构与命令处理表
 */

#include <stdint.h>

#include "app_ahmq.h"

// 显示控制指令
typedef struct [[gnu::packed]] app_ahmq_cmd_display {
    app_ahmq_notify_id_t nid;
    char scrn_sw;
    char color;
    char size;
    char text[];
} app_ahmq_cmd_display_t;

// 屏幕填充
typedef struct [[gnu::packed]] app_ahmq_cmd_fill {
    app_ahmq_notify_id_t nid;
    char color;
} app_ahmq_cmd_fill_t;

// 重启
typedef struct [[gnu::packed]] app_ahmq_cmd_restart {
    app_ahmq_notify_id_t nid;
    char type;
    char set;
    char time[14];
    char reserve;
} app_ahmq_cmd_restart_t;

// 对时
typedef struct [[gnu::packed]] app_ahmq_cmd_checktime {
    app_ahmq_notify_id_t nid;
    char type;
    char time[14];
} app_ahmq_cmd_checktime_t;

/**
 * 命令处理函数：与探针一致地回传协议对象，派生实现用 container_of 取回自身状态。
 * 命令分类由探针在投递当下给出并经 app_dispatch_msg_t.aux 传来，处理函数不必再解析来源主题。
 */
typedef void (*app_ahmq_cmd_handler_fn_t)(app_pcb_t *self, app_ccb_t *ccb, char *data);

/**
 * 一条命令 = 订阅主题 + 处理函数。
 *
 * 主题与处理函数放在同一张表里，**索引即命令号**：通道按它订阅、探针按它分类、
 * 任务按它分派。此前主题在通道与协议里各写一份（订阅 "ASK/x" 却按 "/ASK/x" 分类），
 * 改一条命令要动两处、不一致时还会静默失配；合并成一张表后不可能再漂移。
 */
typedef struct {
    const char               *topic; /**< 订阅主题；由通道负责订阅 */
    app_ahmq_cmd_handler_fn_t  handler;
} app_ahmq_cmd_entry_t;

#define AHMQ_CMD_COUNT (4U)

extern const app_ahmq_cmd_entry_t g_ahmq_cmd_table[AHMQ_CMD_COUNT];
