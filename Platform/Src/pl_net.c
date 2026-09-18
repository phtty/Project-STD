/**
 * @file    pl_net.c
 * @brief   TCP/IP 网络栈抽象（Platform 层）
 *
 * 启动流程：
 *   pl_net_init() → tcpip_init → IP4_ADDR + netif_add(ethernetif_init) → netif_set_up → EthLink 线程
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

/* ================================================================
 *  IP 变更监听器列表 — 上层注册，set_ip 时遍历通知
 * （链路状态监听器已删除，见 pl_net_init 的说明）
 * ================================================================ */

/* IP 变更监听器（见 pl_net_set_ip 的回调；上电默认值不触发） */
static pl_net_ip_listener_t g_ip_listeners[PL_NET_IP_LISTENER_MAX];

void pl_net_register_ip_listener(pl_net_ip_listener_t listener)
{
    for (int i = 0; i < PL_NET_IP_LISTENER_MAX; i++)
        if (g_ip_listeners[i] == nullptr) {
            g_ip_listeners[i] = listener;
            return;
        }
}

/* ================================================================
 *  全局网络状态
 * ================================================================ */

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
    memset(&attributes, 0x0, sizeof(osThreadAttr_t));
    attributes.name       = "EthLink";
    attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
    attributes.priority   = osPriorityBelowNormal;
    pl_task_new(ethernet_link_thread, &gnetif, &attributes);
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

    /* 通知 IP 变更监听器（调用方任务上下文、同步回调）。
       订阅方据此同步各自持有的镜像（如 IAP 记录里的 net_cfg）。 */
    for (int i = 0; i < PL_NET_IP_LISTENER_MAX; i++)
        if (g_ip_listeners[i])
            g_ip_listeners[i](ip, mask, gw);
}

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, IP_ADDRESS, 4);
    memcpy(mask, NETMASK_ADDRESS, 4);
    memcpy(gw, GATEWAY_ADDRESS, 4);
}
