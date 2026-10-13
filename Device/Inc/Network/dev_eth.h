/**
 * @file    dev_eth.h
 * @brief   以太网设备驱动（DP83848 PHY）
 *
 * initcall 阶段：dev_eth_init() 由 hw_dev_initcall 触发，完成 ETH MAC/DMA 硬件初始化
 * （pl_eth_mac_hw_init）、轮询等 PHY 就绪、初始化 DP83848 并注册链路状态回调。
 * RTOS 启动后：dev_eth_start() 由 InitTask 调用，初始化 LwIP 协议栈（pl_net_init）。
 */

#pragma once

#include <stdint.h>

void dev_eth_init(void);    /**< initcall 阶段（ETH MAC/DMA + DP83848 PHY 初始化） */
void dev_eth_start(void);   /**< RTOS 启动后调用（初始化 LwIP + netif） */
