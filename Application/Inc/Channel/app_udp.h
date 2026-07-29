/**
 * @file    app_udp.h
 * @brief   UDP 广播接收通道
 *
 * 默认监听端口 10011，同口承载：
 *   - 创迪发现口（LDI 21H/12H）
 *   - IAP 固件升级（0x5A5A5A5A）
 * 端口可改，但出厂联调默认保持 10011。
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief UDP 通道子类（每 bind 实例） */
typedef struct {
    channel_t me;
    void *conn; /**< 不透明句柄（中间件 netconn），在 .c 中 cast 回具体类型 */
    uint16_t listen_port;
    uint8_t src_ip[4]; /**< 源 IP 地址（IPv4 字节数组） */
    uint16_t src_port;
} udp_channel_t;

extern const ch_ops_t udp_ch_ops;
extern channel_t g_udp_channel_tmpl;

extern osThreadId_t udp_task_handle;
extern const osThreadAttr_t udp_task_attr;

void udp_task(void *argument);

static inline osThreadId_t app_udp_start(void)
{
    return osThreadNew(udp_task, NULL, &udp_task_attr);
}

void app_udp_set_port(uint16_t port);
uint16_t app_udp_get_port(void);
void app_udp_broadcast(const uint8_t *data, uint16_t len);
