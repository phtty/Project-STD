/**
 * @file    pl_spi.h
 * @brief   SPI 平台层抽象（W25Qxx 字库 Flash）
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/** @brief SPI 不透明句柄 */
typedef void *pl_spi_handle_t;

/** @brief DMA 接收完成回调
 *  @param ctx 注册时透传的用户上下文 */
typedef void (*pl_spi_rx_done_fn_t)(void *ctx);

/** @brief 初始化 SPI 外设与 DMA 通道 */
void     pl_spi_init(void);

/** @brief 取 SPI 外设句柄
 *  @return SPI 句柄（恒非 NULL） */
pl_spi_handle_t pl_spi_get_handle(void);

/** @brief 阻塞发送（用于命令/寄存器操作） */
int32_t  pl_spi_transmit(pl_spi_handle_t h, const uint8_t *data, uint16_t size);

/** @brief 阻塞全双工收发（命令+响应一步完成，无残留字节问题） */
int32_t  pl_spi_transmit_receive(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size);

/** @brief DMA 全双工收发 */
int32_t  pl_spi_transmit_receive_dma(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size);

/** @brief 阻塞接收 */
int32_t  pl_spi_receive(pl_spi_handle_t h, uint8_t *data, uint16_t size);

/** @brief DMA 半双工接收（先发命令，再 DMA 收数据直入 buf） */
int32_t  pl_spi_receive_dma(pl_spi_handle_t h, uint8_t *data, uint16_t size);

/** @brief 注册 DMA 接收完成回调（替换式注册）
 *  @param h   SPI 句柄
 *  @param cb  完成回调；传 NULL 表示注销
 *  @param ctx 回调透传的用户上下文 */
void     pl_spi_set_rx_done_fn(pl_spi_handle_t h, pl_spi_rx_done_fn_t cb, void *ctx);
