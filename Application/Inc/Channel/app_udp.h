/**
 * @file    app_udp.h
 * @brief   UDP 广播接收通道（Device 层）
 *
 * 监听 UDP 广播，用于 IAP 固件升级协议。
 * 端口 10011 硬编码，不可修改（升级通道必须保持可连接）。
 *
 * 容器：typedef struct { app_ccb_t base; void *conn; ... } app_udp_ccb_t;
 * base 必须是第一个成员 —— 调用方持有的 app_ccb_t* 与派生指针同址。
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief UDP 通道子类（静态单例：连接信息挂在控制块上，断线只清 conn） */
typedef struct {
    app_ccb_t base;        /**< 第一个成员：container_of 还原 */
    void *conn;        /**< 不透明句柄（中间件 netconn），未连接时为 nullptr */
    uint16_t listen_port;
    uint8_t src_ip[4]; /**< 最近一次收到的源 IP：send 传 nullptr dst 时的回复目标 */
    uint16_t src_port;
} app_udp_ccb_t;

extern const app_ccb_ops_t g_udp_ccb_ops;

extern osThreadId_t g_udp_task_handle;
extern const osThreadAttr_t g_udp_task_attr;

void app_udp_task(void *argument);

static inline osThreadId_t app_udp_start(void)
{
    return osThreadNew(app_udp_task, NULL, &g_udp_task_attr);
}

void app_udp_set_port(uint16_t port);
uint16_t app_udp_get_port(void);
void app_udp_broadcast(const uint8_t *data, uint16_t len);

/** @brief 暴露本通道控制块（协议绑定时使用）*/
app_ccb_t *app_udp_ccb(void);

/** @brief 收/发帧计数（诊断用）
 *
 *  为什么要计数器而不是只看日志：RTT 输出会被后续的周期诊断刷掉，
 *  而"上位机说连上了"这个判断**可能根本不依赖设备应答**。计数是抗截断的证据 ——
 *  上位机说成功而这里仍为 0，说明它判的压根不是设备有没有回。 */
uint32_t app_udp_get_rx_count(void);
uint32_t app_udp_get_tx_count(void);
