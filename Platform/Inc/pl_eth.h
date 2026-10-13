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
/** @brief 一组 RMII 引脚（端口 + 掩码 + 复用号）*/
typedef struct {
    pl_gpio_port_t port;      /**< 该组引脚所在端口 */
    uint16_t  pins;      /**< 该组的引脚掩码，可多脚同组 */
    uint8_t   alternate; /**< 复用功能编号 */
} pl_eth_pin_grp_t;

extern const pl_eth_pin_grp_t g_pl_eth_pin_grps[];    /**< 板级 RMII 引脚分组表 */
extern const uint8_t          g_pl_eth_pin_grp_count; /**< 分组数 */

/* ---- ETH MAC 硬件初始化（Platform 层职责）---- */
/** @brief 初始化 ETH MAC 硬件（引脚、时钟、DMA）*/
void pl_eth_mac_hw_init(void);

/* ---- ETH MAC 暴露给 Device 层的接口 ---- */
/** @brief LwIP netif 初始化回调：填 netif 字段并初始化底层硬件
 *  @param[in,out] netif 待填充的 netif（读改 flags/hwaddr 等）
 *  @return ERR_OK 初始化成功；ERR_MEM 内存不足 */
err_t pl_eth_netif_init(struct netif *netif);
/** @brief ETH 接收线程入口：等 RX 信号量并投递 pbuf 到协议栈 */
void  pl_eth_netif_input(void *argument);
/** @brief 链路监控线程入口：定期轮询 PHY 并按结果起停 MAC/netif */
void  pl_eth_link_task(void *argument);
/** @brief 致命错误处理：关中断并死循环（供断言/初始化失败调用）*/
void  Error_Handler(void);
/** @brief 系统滴答（LwIP sys_jiffies 桩）
 *  @return 当前系统滴答（毫秒）*/
u32_t sys_jiffies(void);
/** @brief 当前系统时间（LwIP sys_now）
 *  @return 自启动起的毫秒数 */
u32_t sys_now(void);

/* ---- MDIO 接口函数（供 Device 层 PHY 驱动调用）---- */
/** @brief 初始化 MDIO 接口（时钟范围等）
 *  @return 0 成功，负值失败 */
int32_t pl_eth_phy_io_init(void);
/** @brief 反初始化 MDIO 接口（当前为空操作）
 *  @return 0 成功，负值失败 */
int32_t pl_eth_phy_io_deinit(void);
/** @brief 通过 MDIO 读取 PHY 寄存器
 *  @param dev_addr PHY 地址
 *  @param reg_addr PHY 寄存器地址
 *  @param[out] reg_val 接收读到的寄存器值
 *  @return 0 成功，负值失败 */
int32_t pl_eth_phy_io_read_reg(uint32_t dev_addr, uint32_t reg_addr, uint32_t *reg_val);
/** @brief 通过 MDIO 写入 PHY 寄存器
 *  @return 0 成功，负值失败 */
int32_t pl_eth_phy_io_write_reg(uint32_t dev_addr, uint32_t reg_addr, uint32_t reg_val);
/** @brief 取系统时间戳（供 PHY 驱动内部定时使用）
 *  @return 当前毫秒数 */
int32_t pl_eth_phy_io_get_tick(void);

/* ---- 链路状态回调注册（Device 层 PHY 初始化后调用）---- */
/** @brief 注册 PHY 链路状态查询函数（Device 层 PHY 初始化后调用）*/
void pl_eth_set_phy_link_fn(pl_phy_link_fn_t fn);
