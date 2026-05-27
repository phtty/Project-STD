/**
 * @file    pl_spi.c
 * @brief   SPI 平台层实现（封装 hspi1）
 */

#include "pl_spi.h"
#include "spi.h"
#include "initcall.h"

typedef struct {
    SPI_HandleTypeDef *hspi;
    pl_spi_rx_cplt_cb_t rx_cplt_cb;
    void *rx_cplt_ctx;
} spi_ctx_t;

static spi_ctx_t g_spi_ctx;

void pl_spi_init(void)
{
    MX_SPI1_Init();
}
hw_device_initcall(pl_spi_init);

pl_spi_handle_t pl_spi_get_handle(void)
{
    g_spi_ctx.hspi = &hspi1;
    return &g_spi_ctx;
}

int32_t pl_spi_transmit_receive_dma(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;
    return (HAL_SPI_TransmitReceive_DMA(ctx->hspi, (uint8_t *)tx_data, (uint8_t *)rx_data, size) == HAL_OK) ? 0 : -1;
}

void pl_spi_set_rx_cplt_cb(pl_spi_handle_t h, pl_spi_rx_cplt_cb_t cb, void *ctx_arg)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (ctx) {
        ctx->rx_cplt_cb   = cb;
        ctx->rx_cplt_ctx  = ctx_arg;
    }
}

/* SPI DMA 完成回调 → 转发到 Device 层 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1 && g_spi_ctx.rx_cplt_cb)
        g_spi_ctx.rx_cplt_cb(g_spi_ctx.rx_cplt_ctx);
}

/* ---- SPI ISR (从 stm32f4xx_it.c 迁移) ---- */
void SPI1_IRQHandler(void) { HAL_SPI_IRQHandler(&hspi1); }
