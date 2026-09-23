/**
 * @file    app_tcp_client.h
 * @brief   TCP 客户端通道（Device 层）
 *
 * 连接远程 TCP 服务器，接收数据通过 app_ccb_dispatch 写入调度框架。
 * 远端地址是模块配置（app_tcp_client_set_remote），不在此写死。
 *
 * 容器：typedef struct { app_ccb_t base; void *conn; } app_tcp_ccb_t;（与 server 共用）
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "app_dispatch.h"

/* 复用 app_tcp_server.h 中的 g_tcp_ccb_ops / app_tcp_ccb_t */
#include "app_tcp_server.h"

extern osThreadId_t g_tcp_client_task_handle;
extern const osThreadAttr_t g_tcp_client_task_attr;

void app_tcp_client_task(void *argument);

static inline osThreadId_t app_tcp_client_start(void)
{
    return osThreadNew(app_tcp_client_task, NULL, &g_tcp_client_task_attr);
}

void app_tcp_client_set_remote(const uint8_t ip[4], uint16_t port);
uint8_t *app_tcp_client_get_host_ip(void);
uint16_t app_tcp_client_get_host_port(void);

/** @brief 暴露本通道控制块（协议绑定时使用）*/
app_ccb_t *app_tcp_client_ccb(void);
