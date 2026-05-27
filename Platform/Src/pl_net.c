/**
 * @file    pl_net.c
 * @brief   TCP/IP 网络栈 Platform 层抽象
 *
 * 集成原 LWIP/App/lwip.c 的 LwIP 初始化逻辑 + Flash 配置读取。
 * 低层 ETH 驱动由 Platform/Src/pl_ethif.c（原 ethernetif.c）提供。
 */

#include "pl_net.h"
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "ethernetif.h"
#include "dev_flash_iap.h"
#include <string.h>

/* ---- 网络状态 ---- */
struct netif gnetif;
ip4_addr_t ipaddr, netmask, gw;
uint8_t IP_ADDRESS[4];
uint8_t NETMASK_ADDRESS[4];
uint8_t GATEWAY_ADDRESS[4];

#define INTERFACE_THREAD_STACK_SIZE (1024)
osThreadAttr_t attributes;

static void ethernet_link_status_updated(struct netif *netif);

void pl_net_init(void)
{
    /* 从 Flash 配置扇区读取 IP 配置 */
    dev_flash_iap_sys_info_t *pConfig = (dev_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR;
    IP_ADDRESS[0]  = pConfig->net_cfg.ip[0];
    IP_ADDRESS[1]  = pConfig->net_cfg.ip[1];
    IP_ADDRESS[2]  = pConfig->net_cfg.ip[2];
    IP_ADDRESS[3]  = pConfig->net_cfg.ip[3];
    NETMASK_ADDRESS[0] = pConfig->net_cfg.mask[0];
    NETMASK_ADDRESS[1] = pConfig->net_cfg.mask[1];
    NETMASK_ADDRESS[2] = pConfig->net_cfg.mask[2];
    NETMASK_ADDRESS[3] = pConfig->net_cfg.mask[3];
    GATEWAY_ADDRESS[0] = pConfig->net_cfg.gw[0];
    GATEWAY_ADDRESS[1] = pConfig->net_cfg.gw[1];
    GATEWAY_ADDRESS[2] = pConfig->net_cfg.gw[2];
    GATEWAY_ADDRESS[3] = pConfig->net_cfg.gw[3];

    tcpip_init(NULL, NULL);

    IP4_ADDR(&ipaddr, IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    IP4_ADDR(&netmask, NETMASK_ADDRESS[0], NETMASK_ADDRESS[1], NETMASK_ADDRESS[2], NETMASK_ADDRESS[3]);
    IP4_ADDR(&gw, GATEWAY_ADDRESS[0], GATEWAY_ADDRESS[1], GATEWAY_ADDRESS[2], GATEWAY_ADDRESS[3]);

    netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);
    netif_set_link_callback(&gnetif, ethernet_link_status_updated);

    memset(&attributes, 0x0, sizeof(osThreadAttr_t));
    attributes.name       = "EthLink";
    attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
    attributes.priority   = osPriorityBelowNormal;
    osThreadNew(ethernet_link_thread, &gnetif, &attributes);

    ethernet_link_status_updated(&gnetif);
}

static void ethernet_link_status_updated(struct netif *netif)
{
    if (netif_is_up(netif) && netif_is_link_up(netif)) {
        printf("Network Ready! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
    } else {
        printf("Network Break!\n");
    }
}
