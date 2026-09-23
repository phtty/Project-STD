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

#define PL_NET_IP_LISTENER_MAX   4

/** @brief IP 变更监听器：pl_net_set_ip 成功后以新值同步回调（调用方任务上下文） */
typedef void (*pl_net_ip_listener_fn_t)(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);

void pl_net_init(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gateway[4]);

/** @brief 注册 IP 变更监听器（如 IAP 记录镜像同步）
 *
 *  **上电默认值不触发** —— 只有 pl_net_set_ip 才会回调。需要"上电也对账一次"
 *  的订阅方（比如没有 set_ip 调用的上电场景）应在自己那侧做初始对账。 */
void pl_net_register_ip_listener(pl_net_ip_listener_fn_t listener);

/** @brief 运行时修改 IP 地址、子网掩码、默认网关 */
void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);

/** @brief 读取当前 IP 地址、子网掩码、默认网关 */
void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
