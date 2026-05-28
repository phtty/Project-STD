/**
 * @file    pl_spi.h
 * @brief   SPI 平台层抽象（W25Qxx 字库 Flash）
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void *pl_spi_handle_t;

typedef void (*pl_spi_rx_cplt_cb_t)(void *ctx);

void     pl_spi_init(void);
pl_spi_handle_t pl_spi_get_handle(void);

/** @brief 阻塞发送（用于命令/寄存器操作） */
int32_t  pl_spi_transmit(pl_spi_handle_t h, const uint8_t *data, uint16_t size);

/** @brief DMA 全双工收发 */
int32_t  pl_spi_transmit_receive_dma(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size);

/** @brief 阻塞接收 */
int32_t  pl_spi_receive(pl_spi_handle_t h, uint8_t *data, uint16_t size);

void     pl_spi_set_rx_cplt_cb(pl_spi_handle_t h, pl_spi_rx_cplt_cb_t cb, void *ctx);
