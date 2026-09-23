/**
 * @file    dev_eth.h
 * @brief   以太网设备驱动（DP83848 PHY）
 *
 * initcall 阶段：dev_eth_init() 由 board_init 调用，预留硬件初始化。
 * RTOS 启动后：dev_eth_start() 由 InitTask 调用，初始化 LwIP 协议栈。
 */

#pragma once

#include <stdint.h>

void dev_eth_init(void);    /**< initcall 阶段（预留 ETH 硬件初始化） */
void dev_eth_start(void);   /**< RTOS 启动后调用（初始化 LwIP + netif） */
