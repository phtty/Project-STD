/**
 * @file    pl_net.h
 * @brief   TCP/IP 网络栈抽象（Platform 层）
 *
 * 提供 pl_net_init()、IP 配置和链路状态回调注册。
 * 对上层暴露注册接口，不直接依赖 Device 层。
 * 不含任何 middleware 或 HAL 类型——只有 stdint/stdbool。
 */

#pragma once

#include <stdint.h>

#define PL_NET_LINK_LISTENER_MAX 8

/** @brief 链路状态监听器，link_up=true 链路恢复，false 链路断开 */
typedef void (*pl_net_link_listener_t)(bool link_up);

void pl_net_init(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gateway[4]);
void pl_net_register_link_listener(pl_net_link_listener_t listener);

/** @brief 运行时修改 IP 地址、子网掩码、默认网关 */
void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);

/** @brief 读取当前 IP 地址、子网掩码、默认网关 */
void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
