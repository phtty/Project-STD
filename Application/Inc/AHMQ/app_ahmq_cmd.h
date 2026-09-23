#pragma once

/**
 * @file    app_ahmq_cmd.h
 * @brief   AHMQ 命令定义：各命令载荷结构与命令处理表
 */

#include <stdint.h>

#include "app_ahmq.h"

/** @brief 显示控制指令载荷（屏幕开关 / 颜色 / 字号 + UTF-8 文本） */
typedef struct [[gnu::packed]] app_ahmq_cmd_display {
    app_ahmq_notify_id_t nid; /**< 通知号 */
    char scrn_sw;             /**< 屏幕开关 */
    char color;               /**< 颜色编码（ASCII，减 '0' 索引颜色表） */
    char size;                /**< 字号编码（ASCII，减 '0' 索引字号表） */
    char text[];              /**< UTF-8 文本内容 */
} app_ahmq_cmd_display_t; /**< 显示控制指令类型 */

/** @brief 屏幕填充指令载荷 */
typedef struct [[gnu::packed]] app_ahmq_cmd_fill {
    app_ahmq_notify_id_t nid; /**< 通知号 */
    char color;               /**< 填充颜色编码（ASCII，减 '0' 索引颜色表） */
} app_ahmq_cmd_fill_t; /**< 屏幕填充指令类型 */

/** @brief 重启指令载荷 */
typedef struct [[gnu::packed]] app_ahmq_cmd_restart {
    app_ahmq_notify_id_t nid; /**< 通知号 */
    char type;                /**< 重启类型 */
    char set;                 /**< 设置标志 */
    char time[14];            /**< 时间字符串（14 字节） */
    char reserve;             /**< 保留字节 */
} app_ahmq_cmd_restart_t; /**< 重启指令类型 */

/** @brief 对时指令载荷 */
typedef struct [[gnu::packed]] app_ahmq_cmd_checktime {
    app_ahmq_notify_id_t nid; /**< 通知号 */
    char type;                /**< 类型：'1' 时应用对时时间 */
    char time[14];            /**< 日期时间（app_ahmq_notify_date_t） */
} app_ahmq_cmd_checktime_t; /**< 对时指令类型 */

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
    app_ahmq_cmd_handler_fn_t  handler; /**< 命令处理函数 */
} app_ahmq_cmd_entry_t;

#define AHMQ_CMD_COUNT (4U) /**< 命令表项数 */

extern const app_ahmq_cmd_entry_t g_ahmq_cmd_table[AHMQ_CMD_COUNT]; /**< 命令表：索引即命令号 */
