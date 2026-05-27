/**
 * @file    dev_tcp_server.h
 * @brief   TCP 服务器通道（Device 层）
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "dispatch_types.h"

/** @brief TCP Server 监听方上下文（每端口一个） */
typedef struct {
    uint16_t port;      /**< 监听端口 */
    uint8_t conn_count; /**< 当前连接数 */
    uint8_t max_conns;  /**< 最大连接数 */
    bool restart;       /**< set_port 通知监听任务自行重启 */
    void *listen_conn;  /**< 不透明句柄（中间件 netconn），在 .c 中 cast 回具体类型 */
} tcp_server_listener_ctx_t;

/** @brief TCP Server 通道子类（每连接实例） */
typedef struct {
    channel_t ch;
    void *conn;                          /**< 不透明句柄（中间件 netconn），在 .c 中 cast 回具体类型 */
    tcp_server_listener_ctx_t *listener; /**< 所属监听器（用于断开时 conn_count--） */
} tcp_server_channel_t;

extern const ch_ops_t tcp_ch_ops;
extern channel_t g_tcp_server_channel_tmpl;

extern osThreadId_t tcp_server_task_handle;
extern const osThreadAttr_t tcp_server_task_attr;

void tcp_server_channel_init(tcp_server_channel_t *self, void *conn, channel_t *tmpl, tcp_server_listener_ctx_t *listener);
void tcp_server_channel_deinit(tcp_server_channel_t *self);
void tcp_server_task(void *argument);
void tcp_server_listen_task(void *ctx);

static inline osThreadId_t app_tcp_server_start(void)
{
    return osThreadNew(tcp_server_task, NULL, &tcp_server_task_attr);
}
void app_tcp_server_set_port(uint16_t port);
uint16_t app_tcp_server_get_port(void);
