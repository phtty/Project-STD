/**
 * @file    app_tcp_server.h
 * @brief   TCP 服务器通道 — manage 任务 + conn 任务
 *
 * tcp_server_task: bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 * tcp_server_conn_task: netconn_recv → app_ccb_dispatch
 *
 * tcp_ccb_t 被 TCP 服务端与客户端共用：两者形状一致（基类 + conn），
 * 因而共用同一张 ccb_ops 虚表，container_of 对二者都成立。
 * base 必须是第一个成员 —— 调用方持有的 ccb_t* 与派生指针同址。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief netconn 类通道子类（TCP server / client 共用） */
typedef struct {
    ccb_t base; /**< 第一个成员：container_of 还原 */
    void *conn; /**< 不透明句柄（netconn），断开时为 nullptr */
} tcp_ccb_t;

extern const ccb_ops_t tcp_ccb_ops;

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

/** @brief 暴露本通道控制块（协议绑定时使用）*/
ccb_t *app_tcp_server_ccb(void);
