/**
 * @file    pl_net.c
 * @brief   TCP/IP 网络栈抽象（Platform 层）
 *
 * 启动流程：
 *   pl_net_init() → tcpip_init → IP4_ADDR + netif_add(ethernetif_init) → netif_set_up → EthLink 线程
 *
 * 链路状态通过回调通知上层（DIP：Platform 不直接依赖 Device）。
 * IP 配置通过 tcpip_callback 投递到 TCP/IP 线程安全执行。
 */

#include "pl_net.h"
#include "pl_net_adapt.h"
#include "pl_eth.h"
#include <string.h>

/* ================================================================
 *  链路状态监听器列表 — 上层注册，链路变化时遍历通知
 * ================================================================ */

static pl_net_link_listener_t g_link_listeners[PL_NET_LINK_LISTENER_MAX];

void pl_net_register_link_listener(pl_net_link_listener_t listener)
{
    for (int i = 0; i < PL_NET_LINK_LISTENER_MAX; i++)
        if (g_link_listeners[i] == nullptr) {
            g_link_listeners[i] = listener;
            return;
        }
}

/* ================================================================
 *  全局网络状态
 * ================================================================ */

static void ethernet_link_status_updated(struct netif *netif);
void Error_Handler(void);

struct netif gnetif;            /* 网络接口（全局，供 IP 配置 API 使用） */
ip4_addr_t ipaddr, netmask, gw; /* ip4_addr_t 格式 */
uint8_t IP_ADDRESS[4];          /* 字节数组格式（供 get_ip 读取） */
uint8_t NETMASK_ADDRESS[4];
uint8_t GATEWAY_ADDRESS[4];

#define INTERFACE_THREAD_STACK_SIZE (1024)
osThreadAttr_t attributes;

/* ================================================================
 *  协议栈初始化（在 init_task 中调用，必须在 RTOS 启动之后）
 * ================================================================ */

void pl_net_init(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gateway[4])
{
    memcpy(IP_ADDRESS, ip, 4);
    memcpy(NETMASK_ADDRESS, mask, 4);
    memcpy(GATEWAY_ADDRESS, gateway, 4);

    tcpip_init(NULL, nullptr); /* 启动 TCP/IP 线程 */

    IP4_ADDR(&ipaddr, IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    IP4_ADDR(&netmask, NETMASK_ADDRESS[0], NETMASK_ADDRESS[1], NETMASK_ADDRESS[2], NETMASK_ADDRESS[3]);
    IP4_ADDR(&gw, GATEWAY_ADDRESS[0], GATEWAY_ADDRESS[1], GATEWAY_ADDRESS[2], GATEWAY_ADDRESS[3]);

    /* 注册 netif：ethernetif_init 作为 low_level_init 回调，配置 MAC/PHY/DMA */
    netif_add(&gnetif, &ipaddr, &netmask, &gw, nullptr, &ethernetif_init, &tcpip_input);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);
    netif_set_link_callback(&gnetif, ethernet_link_status_updated); /* 链路变化通知 */

    /* 启动 EthLink 线程（每 100ms 轮询 PHY 链路状态） */
    memset(&attributes, 0x0, sizeof(osThreadAttr_t));
    attributes.name       = "EthLink";
    attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
    attributes.priority   = osPriorityBelowNormal;
    osThreadNew(ethernet_link_thread, &gnetif, &attributes);
}

/* ================================================================
 *  链路状态回调 — link up 时通知所有监听器
 * ================================================================ */

static void ethernet_link_status_updated(struct netif *netif)
{
    if (netif_is_up(netif) && netif_is_link_up(netif)) {
        bool link_up = netif_is_up(netif) && netif_is_link_up(netif);
        for (int i = 0; i < PL_NET_LINK_LISTENER_MAX; i++)
            if (g_link_listeners[i])
                g_link_listeners[i](link_up);
    }
}

/* ================================================================
 *  运行时 IP 配置（通过 tcpip_callback 投递到 TCP/IP 线程，线程安全）
 * ================================================================ */

static ip4_addr_t g_new_ip, g_new_mask, g_new_gw;

static void apply_ip_config(void *ctx)
{
    (void)ctx;
    netif_set_addr(&gnetif, &g_new_ip, &g_new_mask, &g_new_gw);
}

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    IP4_ADDR(&g_new_ip, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&g_new_mask, mask[0], mask[1], mask[2], mask[3]);
    IP4_ADDR(&g_new_gw, gw[0], gw[1], gw[2], gw[3]);

    memcpy(IP_ADDRESS, ip, 4);
    memcpy(NETMASK_ADDRESS, mask, 4);
    memcpy(GATEWAY_ADDRESS, gw, 4);

    tcpip_callback(apply_ip_config, nullptr); /* 线程安全：投递到 TCP/IP 线程执行 */
}

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, IP_ADDRESS, 4);
    memcpy(mask, NETMASK_ADDRESS, 4);
    memcpy(gw, GATEWAY_ADDRESS, 4);
}
