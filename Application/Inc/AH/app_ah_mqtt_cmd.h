#pragma once

/**
 * @file    app_ah_mqtt_cmd.h
 * @brief   AH/MQTT 命令定义：各命令载荷结构与命令处理表
 */

#include <stdint.h>

#include "app_ah_mqtt.h"

/** @brief 显示控制指令载荷（屏幕开关 / 颜色 / 字号 + UTF-8 文本） */
typedef struct [[gnu::packed]] app_ah_mqtt_cmd_display {
    app_ah_notify_id_t notify; /**< 通知号 */
    char scrn_sw;             /**< 屏幕开关 */
    char color;               /**< 颜色编码（ASCII，减 '0' 索引颜色表） */
    char size;                /**< 字号编码（ASCII，减 '0' 索引字号表） */
    char text[];              /**< UTF-8 文本内容 */
} app_ah_mqtt_cmd_display_t; /**< 显示控制指令类型 */

/** @brief 屏幕填充指令载荷 */
typedef struct [[gnu::packed]] app_ah_mqtt_cmd_fill {
    app_ah_notify_id_t notify; /**< 通知号 */
    char color;               /**< 填充颜色编码（ASCII，减 '0' 索引颜色表） */
} app_ah_mqtt_cmd_fill_t; /**< 屏幕填充指令类型 */

/** @brief 重启指令载荷 */
typedef struct [[gnu::packed]] app_ah_mqtt_cmd_restart {
    app_ah_notify_id_t notify; /**< 通知号 */
    char type;                /**< 重启类型 */
    char set;                 /**< 设置标志 */
    char time[14];            /**< 时间字符串（14 字节） */
    char reserve;             /**< 保留字节 */
} app_ah_mqtt_cmd_restart_t; /**< 重启指令类型 */

/** @brief 对时指令载荷 */
typedef struct [[gnu::packed]] app_ah_mqtt_cmd_checktime {
    app_ah_notify_id_t notify; /**< 通知号 */
    char type;                /**< 类型：'1' 时应用对时时间 */
    char time[14];            /**< 日期时间 YYYYMMDDhhmmss */
} app_ah_mqtt_cmd_checktime_t; /**< 对时指令类型 */

/**
 * 命令处理函数：与探针一致地回传协议对象，派生实现用 container_of 取回自身状态。
 * 命令分类由探针在投递当下给出并经 app_dispatch_msg_t.aux 传来，处理函数不必再解析来源主题。
 */
typedef void (*app_ah_mqtt_cmd_handler_fn_t)(app_pcb_t *self, app_ccb_t *ccb, char *data);

/**
 * 一条命令 = 层级后缀 + 处理函数。
 *
 * 表里只存**与设备身份无关的后缀**（`ASK/board/NULL` 等），完整主题在
 * `app_ah_mqtt` 的 sw_post initcall 里用配置拼出来：
 * `前缀/后缀`。索引即命令号：探针按完整主题分类、任务按它分派。
 *
 * 此前表里存完整主题、探针却拿另一份字符串比较，订阅和分类各写一份，
 * 不一致时静默失配；现在只有一处后缀来源。
 */
typedef struct {
    const char                   *suffix; /**< 层级后缀（如 "ASK/board/NULL"） */
    app_ah_mqtt_cmd_handler_fn_t  handler; /**< 命令处理函数 */
} app_ah_mqtt_cmd_entry_t;

extern const app_ah_mqtt_cmd_entry_t g_ah_mqtt_cmd_table[AH_MQTT_CMD_COUNT]; /**< 命令表：索引即命令号 */
