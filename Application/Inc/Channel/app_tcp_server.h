/**
 * @file    app_tcp_server.h
 * @brief   TCP 服务器通道 — 可实例化的 manage 任务 + conn 任务
 *
 * 每个实例 = 一个监听端口 + 一份通道控制块：
 *   app_tcp_server_task: bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 *   app_tcp_server_conn_task: netconn_recv → app_ccb_dispatch
 *
 * 默认实例（端口 9528）保持原有 API：`app_tcp_server_ccb()` / `app_tcp_server_start()`，
 * LDI 不需要改。AH/TCP 参数口用 `app_tcp_server_open()` 再开一个实例（6001）。
 *
 * app_tcp_ccb_t 被 TCP 服务端与客户端共用：两者形状一致（基类 + conn），
 * 因而共用同一张 ccb_ops 虚表，container_of 对二者都成立。
 * base 必须是第一个成员 —— 调用方持有的 app_ccb_t* 与派生指针同址。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief 服务端实例（不透明；静态池分配，不释放） */
typedef struct app_tcp_server app_tcp_server_t;

/** @brief netconn 类通道子类（TCP server / client 共用） */
typedef struct {
    app_ccb_t base; /**< 第一个成员：container_of 还原 */
    void *conn; /**< 不透明句柄（netconn），断开时为 nullptr */
} app_tcp_ccb_t;

extern const app_ccb_ops_t g_tcp_ccb_ops; /**< netconn 类通道 ops 虚表（server/client 共用） */

extern osThreadId_t g_tcp_server_task_handle;       /**< 默认实例的管理任务句柄 */
extern const osThreadAttr_t g_tcp_server_task_attr; /**< 管理任务属性（所有实例共用） */

/** @brief 服务端管理任务：bind → listen → accept → 派生 conn 任务 → 等待断开 → 循环
 *  @param argument 服务端实例（`app_tcp_server_t *`） */
void app_tcp_server_task(void *argument);

/** @brief 服务端连接任务：netconn_recv → app_ccb_dispatch，断开时释放信号量
 *  @param argument 服务端实例（`app_tcp_server_t *`）；连接句柄挂在实例的 ccb 上 */
void app_tcp_server_conn_task(void *argument);

/** @brief 开一个服务端实例（只初始化，不启动任务）
 *  @param name          通道名（只读；须为静态字符串，用于排障）
 *  @param port          监听端口（0 非法）
 *  @param idle_timeout_ms 连接空闲超时（毫秒）；0 = 不超时（默认实例的既有行为）
 *  @return 实例指针；池满/参数非法返回 nullptr
 *  @note 池大小 2：默认实例 + 一个扩展实例；调用后再 `app_tcp_server_start_instance()`。 */
app_tcp_server_t *app_tcp_server_open(const char *name, uint16_t port, uint32_t idle_timeout_ms);

/** @brief 启动一个服务端实例的管理任务
 *  @param srv 由 `app_tcp_server_open()` 返回的实例
 *  @return 任务句柄；失败返回 nullptr */
osThreadId_t app_tcp_server_start_instance(app_tcp_server_t *srv);

/** @brief 取实例的通道控制块（协议绑定时使用）
 *  @param srv 服务端实例
 *  @return 通道控制块；srv 为空返回 nullptr */
app_ccb_t *app_tcp_server_ccb_of(const app_tcp_server_t *srv);

/** @brief 启动默认实例（端口 9528，LDI 使用）
 *  @return 任务句柄；失败返回 nullptr */
osThreadId_t app_tcp_server_start(void);

/** @brief 设置默认实例的监听端口（须在管理任务启动前调用）
 *  @param port 监听端口号 */
void app_tcp_server_set_port(uint16_t port);

/** @brief 读取默认实例的监听端口
 *  @return 监听端口号 */
uint16_t app_tcp_server_get_port(void);

/** @brief 暴露默认实例的通道控制块（协议绑定时使用；不存在则惰性创建）
 *  @return 通道控制块 */
app_ccb_t *app_tcp_server_ccb(void);
