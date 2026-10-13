/**
 * @file    pl_dma.h
 * @brief   DMA 初始化接口（hw_pl_initcall 优先级 2）
 */

#pragma once

/** @brief 按板级配置初始化各 DMA 控制器与流 */
void pl_dma_init(void);
