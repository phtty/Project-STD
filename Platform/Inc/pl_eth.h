#pragma once

/**
 * @file    pl_eth.h
 * @brief   以太网 Platform 抽象：MAC 初始化、netif 接口与 PHY 链路回调
 */

#include "lwip/err.h"
#include "lwip/netif.h"
#include "cmsis_os.h"
#include "pl_gpio.h" /* pl_gpio_port_t（板级引脚表用） */

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

/* ---- 板级表：ETH 的 RMII 引脚分组 ----
 *
 * 共享的 pl_eth.c 只认这张表，本表是板子对"PHY 怎么接"的回答。
 *
 * **为什么要有它**：这段引脚配置原先直接写在 pl_eth.c 的 HAL_ETH_MspInit 里 ——
 * 它是从 CubeMX 的 stm32f4xx_hal_msp.c 搬过来的**板级数据**，却住在共享层。两块板
 * 恰好都是 F407 的标准 RMII 脚位，所以一直没暴露；换一块 PHY 接线不同的板会静默
 * 拿到这几个脚，表现为"网口不通"且没有任何编译期提示。
 *
 * 引脚号在 CubeMX 的 main.h 里没有标签（本工程不再重新生成 CubeMX 代码），
 * 故板级表直接写端口与引脚号。 */
typedef struct {
    pl_gpio_port_t port;      /**< 该组引脚所在端口 */
    uint16_t  pins;      /**< 该组的引脚掩码，可多脚同组 */
    uint8_t   alternate; /**< 复用功能编号 */
} pl_eth_pin_grp_t;

extern const pl_eth_pin_grp_t g_pl_eth_pin_grps[];
extern const uint8_t          g_pl_eth_pin_grp_count;

/* ---- ETH MAC 硬件初始化（Platform 层职责）---- */
void pl_eth_mac_hw_init(void);

/* ---- ETH MAC 暴露给 Device 层的接口 ---- */
err_t pl_eth_netif_init(struct netif *netif);
void  pl_eth_netif_input(void *argument);
void  pl_eth_link_task(void *argument);
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
