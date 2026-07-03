/**
 * @file    app_tcp_server.h
 * @brief   TCP 服务器通道 — manage 任务 + conn 任务
 *
 * tcp_server_task: bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 * tcp_server_conn_task: netconn_recv → app_channel_dispatch
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "dispatch_types.h"

/** @brief TCP Server 通道子类 */
typedef struct {
    channel_t me;
    void *conn; /**< 不透明句柄 (netconn) */
} tcp_server_channel_t;

extern const ch_ops_t tcp_ch_ops;
extern channel_t g_tcp_server_channel_tmpl;

extern osThreadId_t tcp_server_task_handle;
extern const osThreadAttr_t tcp_server_task_attr;

void tcp_server_task(void *argument);
void tcp_server_conn_task(void *argument);

static inline osThreadId_t app_tcp_server_start(void)
{
    return osThreadNew(tcp_server_task, NULL, &tcp_server_task_attr);
}
void app_tcp_server_set_port(uint16_t port);
uint16_t app_tcp_server_get_port(void);
