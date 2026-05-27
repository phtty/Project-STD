#ifndef PL_ETH_MAC_H
#define PL_ETH_MAC_H

#include "lwip/err.h"
#include "lwip/netif.h"
#include "cmsis_os.h"

/** @brief 链路状态（Platform 层抽象，隔离具体 PHY 型号的状态码） */
typedef enum {
    PL_ETH_LINK_DOWN,      /**< 链路断开 */
    PL_ETH_LINK_10M_HD,    /**< 10M 半双工 */
    PL_ETH_LINK_10M_FD,    /**< 10M 全双工 */
    PL_ETH_LINK_100M_HD,   /**< 100M 半双工 */
    PL_ETH_LINK_100M_FD,   /**< 100M 全双工 */
} pl_eth_link_state_t;

/** @brief PHY 链路状态查询函数类型（Device 层注册） */
typedef pl_eth_link_state_t (*pl_phy_link_fn_t)(void);

/* ---- ETH MAC 硬件初始化（Platform 层职责）---- */
void pl_eth_mac_hw_init(void);

/* ---- ETH MAC 暴露给 Device 层的接口 ---- */
err_t ethernetif_init(struct netif *netif);
void  ethernetif_input(void *argument);
void  ethernet_link_thread(void *argument);
void  Error_Handler(void);
u32_t sys_jiffies(void);
u32_t sys_now(void);

/* ---- MDIO 接口函数（供 Device 层 PHY 驱动调用）---- */
int32_t pl_eth_phy_io_init(void);
int32_t pl_eth_phy_io_deinit(void);
int32_t pl_eth_phy_io_read_reg(uint32_t dev_addr, uint32_t reg_addr, uint32_t *reg_val);
int32_t pl_eth_phy_io_write_reg(uint32_t dev_addr, uint32_t reg_addr, uint32_t reg_val);
int32_t pl_eth_phy_io_get_tick(void);

/* ---- 链路状态回调注册（Device 层 PHY 初始化后调用）---- */
void pl_eth_set_phy_link_fn(pl_phy_link_fn_t fn);

#endif
