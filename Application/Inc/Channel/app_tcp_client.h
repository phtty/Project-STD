/**
 * @file    dev_tcp_client.h
 * @brief   TCP 客户端通道（Device 层）
 *
 * 连接远程 TCP 服务器，接收数据通过 app_channel_dispatch 写入调度框架。
 * 默认: 192.168.114.100:9529，断线自动重连。
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "dispatch_types.h"

/* 复用 app_tcp_server.h 中的 tcp_ch_ops */
#include "app_tcp_server.h"

/** @brief TCP Client 通道子类（单例，自带远端配置） */
typedef struct {
    channel_t me;
    void *conn; /**< 不透明句柄（中间件 netconn），在 .c 中 cast 回具体类型 */
    uint8_t host_ip[4];
    uint16_t host_port;
} tcp_client_channel_t;

extern tcp_client_channel_t g_tcp_client;
extern channel_t g_tcp_client_channel_tmpl;

extern osThreadId_t tcp_client_task_handle;
extern const osThreadAttr_t tcp_client_task_attr;

void tcp_client_channel_init(tcp_client_channel_t *self, void *conn, channel_t *tmpl);
void tcp_client_task(void *argument);

static inline osThreadId_t app_tcp_client_start(void)
{
    return osThreadNew(tcp_client_task, NULL, &tcp_client_task_attr);
}

void app_tcp_client_set_remote(const uint8_t ip[4], uint16_t port);
uint8_t *app_tcp_client_get_host_ip(void);
uint16_t app_tcp_client_get_host_port(void);
