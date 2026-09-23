/**
 * @file    app_tcp_server.h
 * @brief   TCP 服务器通道 — manage 任务 + conn 任务
 *
 * app_tcp_server_task: bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 * app_tcp_server_conn_task: netconn_recv → app_ccb_dispatch
 *
 * app_tcp_ccb_t 被 TCP 服务端与客户端共用：两者形状一致（基类 + conn），
 * 因而共用同一张 ccb_ops 虚表，container_of 对二者都成立。
 * base 必须是第一个成员 —— 调用方持有的 app_ccb_t* 与派生指针同址。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief netconn 类通道子类（TCP server / client 共用） */
typedef struct {
    app_ccb_t base; /**< 第一个成员：container_of 还原 */
    void *conn; /**< 不透明句柄（netconn），断开时为 nullptr */
} app_tcp_ccb_t;

extern const app_ccb_ops_t g_tcp_ccb_ops; /**< netconn 类通道 ops 虚表（server/client 共用） */

extern osThreadId_t g_tcp_server_task_handle;       /**< TCP 服务端管理任务句柄 */
extern const osThreadAttr_t g_tcp_server_task_attr; /**< TCP 服务端管理任务属性 */

/** @brief TCP 服务端管理任务：bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 *  @param argument 未使用（单例任务） */
void app_tcp_server_task(void *argument);

/** @brief TCP 服务端连接任务：netconn_recv → app_ccb_dispatch，断开时释放信号量
 *  @param argument accept 得到的 netconn 句柄 */
void app_tcp_server_conn_task(void *argument);

static inline osThreadId_t app_tcp_server_start(void)
{
    return osThreadNew(app_tcp_server_task, NULL, &g_tcp_server_task_attr);
}

/** @brief 设置监听端口（须在管理任务启动前调用）
 *  @param port 监听端口号 */
void app_tcp_server_set_port(uint16_t port);

/** @brief 读取当前监听端口
 *  @return 监听端口号 */
uint16_t app_tcp_server_get_port(void);

/** @brief 暴露本通道控制块（协议绑定时使用）*/
app_ccb_t *app_tcp_server_ccb(void);
