/**
 * @file    pl_net.c
 * @brief   TCP/IP 网络栈抽象（Platform 层）
 *
 * 启动流程：
 *   pl_net_init() → tcpip_init → IP4_ADDR + netif_add(pl_eth_netif_init) → netif_set_up → EthLink 线程
 *
 * IP 配置通过 tcpip_callback 投递到 TCP/IP 线程安全执行。
 *
 * 注意：**没有链路状态通知机制**（原有的那套 2026-09-18 删除，理由见 pl_net_init 里
 * 那段说明）。链路断开由各通道自己感知 —— UDP socket 一直绑着自愈，TCP 靠
 * netconn_recv 返错退出循环重连。
 */

#include "pl_net.h"
#include "pl_net_adapt.h"
#include "pl_eth.h"
#include <string.h>
#include "pl_task.h"
#include "pl_net_diag.h"

/* ================================================================
 *  IP 变更监听器列表 — 上层注册，set_ip 时遍历通知
 * （链路状态监听器已删除，见 pl_net_init 的说明）
 * ================================================================ */

/* IP 变更监听器（见 pl_net_set_ip 的回调；上电默认值不触发） */
static pl_net_ip_listener_fn_t s_ip_listeners[PL_NET_IP_LISTENER_MAX];

void pl_net_register_ip_listener(pl_net_ip_listener_fn_t listener)
{
    for (int i = 0; i < PL_NET_IP_LISTENER_MAX; i++)
        if (s_ip_listeners[i] == nullptr) {
            s_ip_listeners[i] = listener;
            return;
        }
}

/* ================================================================
 *  全局网络状态
 * ================================================================ */

void Error_Handler(void);

struct netif g_net_netif;            /* 网络接口（全局，供 IP 配置 API 使用） */
static uint8_t s_ip_bytes[4];        /* IP 字节数组格式（供 get_ip 读取） */
static uint8_t s_netmask_bytes[4];
static uint8_t s_gateway_bytes[4];

#define ETH_NET_TASK_STACK (1024)
static osThreadAttr_t s_net_thread_attr;

/* ================================================================
 *  协议栈初始化（在 init_task 中调用，必须在 RTOS 启动之后）
 * ================================================================ */

void pl_net_init(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gateway[4])
{
    ip4_addr_t ipaddr, netmask, gw; /* ip4_addr_t 格式（仅本函数使用） */

    memcpy(s_ip_bytes, ip, 4);
    memcpy(s_netmask_bytes, mask, 4);
    memcpy(s_gateway_bytes, gateway, 4);

    tcpip_init(NULL, nullptr); /* 启动 TCP/IP 线程 */

    IP4_ADDR(&ipaddr, s_ip_bytes[0], s_ip_bytes[1], s_ip_bytes[2], s_ip_bytes[3]);
    IP4_ADDR(&netmask, s_netmask_bytes[0], s_netmask_bytes[1], s_netmask_bytes[2], s_netmask_bytes[3]);
    IP4_ADDR(&gw, s_gateway_bytes[0], s_gateway_bytes[1], s_gateway_bytes[2], s_gateway_bytes[3]);

    /* 注册 netif：pl_eth_netif_init 作为 _eth_low_level_init 回调，配置 MAC/PHY/DMA */
    netif_add(&g_net_netif, &ipaddr, &netmask, &gw, nullptr, &pl_eth_netif_init, &tcpip_input);
    netif_set_default(&g_net_netif);
    netif_set_up(&g_net_netif);
    /* 这里曾注册 netif_set_link_callback 做链路状态通知，2026-09-18 删除。
       两个理由：其一，它从来没生效过 —— 回调里判 netif_is_up && netif_is_link_up，
       而链路线程在断开时先 netif_set_down 再 netif_set_link_down，回调进来时
       netif_is_up 已是 false，通知永远发不出去；唯一的使用者 udp_link_listener
       只在 link_up 为 false 时动作，那段是死代码。
       其二，也是更关键的：**它要保护的东西不需要保护**。UDP socket 一直绑着、
       netconn_recv 一直阻塞，实测拔插网线自愈；TCP 通道压根没注册监听器，
       靠 netconn_recv 返回非 OK 退出循环、外层重连。
       而"照原样修好通知"反而更糟：udp_task 的断开路径带 osDelay(2000) 才重新 bind，
       链路短暂抖动会变成 2 秒不听包的盲窗，今天反而是连续的。
       将来真要"断开时主动重建"（例如上报断线状态）再加回来，但必须同时去掉那个盲窗。 */

    /* 启动 EthLink 线程（每 100ms 轮询 PHY 链路状态） */
    memset(&s_net_thread_attr, 0x0, sizeof(osThreadAttr_t));
    s_net_thread_attr.name       = "EthLink";
    s_net_thread_attr.stack_size = ETH_NET_TASK_STACK;
    s_net_thread_attr.priority   = osPriorityBelowNormal;
    pl_task_new(pl_eth_link_task, &g_net_netif, &s_net_thread_attr);

    PL_NET_DIAG("netif 就绪 ip=%u.%u.%u.%u（此刻链路状态由下面的 PHY 日志给出）", ip[0], ip[1], ip[2],
             ip[3]);
}

/* ================================================================
 *  运行时 IP 配置（通过 tcpip_callback 投递到 TCP/IP 线程，线程安全）
 * ================================================================ */

static ip4_addr_t s_new_ip, s_new_mask, s_new_gw;

static void _net_apply_ip_cfg(void *ctx)
{
    (void)ctx;
    netif_set_addr(&g_net_netif, &s_new_ip, &s_new_mask, &s_new_gw);
}

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    IP4_ADDR(&s_new_ip, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&s_new_mask, mask[0], mask[1], mask[2], mask[3]);
    IP4_ADDR(&s_new_gw, gw[0], gw[1], gw[2], gw[3]);

    memcpy(s_ip_bytes, ip, 4);
    memcpy(s_netmask_bytes, mask, 4);
    memcpy(s_gateway_bytes, gw, 4);

    tcpip_callback(_net_apply_ip_cfg, nullptr); /* 线程安全：投递到 TCP/IP 线程执行 */

    /* 这条很关键：设备的"最终 IP"由这里决定（pl_net_init 用的是编译期默认值，
       会被 LDI 配置覆盖）。要与上位机对照是否同网段，看这条。 */
    PL_NET_DIAG("set_ip -> %u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3],
             mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);

    /* 通知 IP 变更监听器（调用方任务上下文、同步回调）。
       订阅方据此同步各自持有的镜像（如 IAP 记录里的 net_cfg）。 */
    for (int i = 0; i < PL_NET_IP_LISTENER_MAX; i++)
        if (s_ip_listeners[i])
            s_ip_listeners[i](ip, mask, gw);
}

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_ip_bytes, 4);
    memcpy(mask, s_netmask_bytes, 4);
    memcpy(gw, s_gateway_bytes, 4);
}
