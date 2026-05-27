/**
 * @file    pl_net_adapt.h
 * @brief   网络中间件适配头文件（Platform 层，仅供 .c 使用）
 *
 * 聚合当前网络中间件（LwIP）的所有 API 头文件。
 * 上层 .c 文件需要网络中间件 API 时，只 include 这一个头文件。
 * 换网络协议栈时，只需改这个文件的 include 列表。
 *
 * **禁止在任何 .h 头文件中 include 此文件** —— 它仅供实现文件使用。
 */

#pragma once

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "lwip/ethip6.h"
#include "lwip/apps/mqtt.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
