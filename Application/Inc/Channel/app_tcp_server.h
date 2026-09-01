/**
 * @file    app_tcp_server.h
 * @brief   TCP 服务器通道 — 串行单客户端服务循环
 *
 * tcp_server_task: bind → listen → accept → 服务客户端（recv 循环）
 * → 客户端断开 → 回 accept 下一客户端。通道实例为文件级 static 单实例
 * （UAF 根治），app_channel_register/dispatch 协议不变。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

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

static inline osThreadId_t app_tcp_server_start(void)
{
    return osThreadNew(tcp_server_task, NULL, &tcp_server_task_attr);
}
void app_tcp_server_set_port(uint16_t port);
uint16_t app_tcp_server_get_port(void);