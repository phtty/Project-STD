/**
 * @file    pl_spi.c
 * @brief   SPI 平台层实现（封装 hspi1）
 */

#include "pl_spi.h"
#include "spi.h"
#include "initcall.h"
#include <stdio.h>
#include "pl_mem.h"

typedef struct {
    SPI_HandleTypeDef *hspi;
    pl_spi_rx_done_fn_t rx_done_fn;
    void *rx_done_ctx;
} spi_ctx_t;

static spi_ctx_t s_spi_ctx;

/** @brief 最近一次 DMA 接收"没起来"的现场
 *
 *  HAL 的失败路径大多**只返回状态码、不留下任何寄存器痕迹**：等事后拿调试器看，
 *  SPI 和两条 DMA 全是 READY/0 —— 看不出是谁把状态机弄脏的。所以在失败的那一刻
 *  把能看到的都抄下来。printf 走 RTT，现场往往没接调试器/被日志冲掉，
 *  这个结构用调试器直接读，不含主观判断。 */
typedef struct {
    volatile uint32_t count;        /**< 失败次数 */
    volatile uint32_t lock_before;  /**< 调用前 HAL 锁：0=未锁 1=已锁 */
    volatile uint32_t size;         /**< 本次请求长度 */
    volatile uint32_t spi_state;    /**< 失败后 hspi->State */
    volatile uint32_t spi_err;      /**< 失败后 hspi->ErrorCode */
    volatile uint32_t tx_state;     /**< 失败后 TX DMA State */
    volatile uint32_t tx_err;       /**< 失败后 TX DMA ErrorCode */
    volatile uint32_t rx_state;     /**< 失败后 RX DMA State */
    volatile uint32_t rx_err;       /**< 失败后 RX DMA ErrorCode */
} pl_spi_fail_t;

static pl_spi_fail_t s_fail;

const void *pl_spi_last_failure(void)
{
    return &s_fail;
}

static void _record_failure(spi_ctx_t *ctx, uint32_t lock_before, uint32_t size)
{
    s_fail.count++;
    s_fail.lock_before = lock_before;
    s_fail.size        = size;
    s_fail.spi_state   = (uint32_t)ctx->hspi->State;
    s_fail.spi_err     = (uint32_t)ctx->hspi->ErrorCode;
    if (ctx->hspi->hdmatx) {
        s_fail.tx_state = (uint32_t)ctx->hspi->hdmatx->State;
        s_fail.tx_err   = ctx->hspi->hdmatx->ErrorCode;
    }
    if (ctx->hspi->hdmarx) {
        s_fail.rx_state = (uint32_t)ctx->hspi->hdmarx->State;
        s_fail.rx_err   = ctx->hspi->hdmarx->ErrorCode;
    }
}

void pl_spi_init(void)
{
    MX_SPI1_Init();
}
hw_pl_initcall(pl_spi_init);

pl_spi_handle_t pl_spi_get_handle(void)
{
    s_spi_ctx.hspi = &hspi1;
    return &s_spi_ctx;
}

int32_t pl_spi_transmit_receive(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;
    return (HAL_SPI_TransmitReceive(ctx->hspi, (uint8_t *)tx_data, (uint8_t *)rx_data, size, 100) == HAL_OK) ? 0 : -1;
}

int32_t pl_spi_transmit_receive_dma(pl_spi_handle_t h, const uint8_t *tx_data, uint8_t *rx_data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;
    return (HAL_SPI_TransmitReceive_DMA(ctx->hspi, (uint8_t *)tx_data, (uint8_t *)rx_data, size) == HAL_OK) ? 0 : -1;
}

void pl_spi_set_rx_done_fn(pl_spi_handle_t h, pl_spi_rx_done_fn_t fn, void *ctx_arg)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (ctx) {
        ctx->rx_done_fn   = fn;
        ctx->rx_done_ctx  = ctx_arg;
    }
}

int32_t pl_spi_transmit(pl_spi_handle_t h, const uint8_t *data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;
    HAL_StatusTypeDef st = HAL_SPI_Transmit(ctx->hspi, (uint8_t *)data, size, 100);
    /* HAL_SPI_Transmit 返回后移位寄存器可能仍在忙，必须等 BSY 清除才能操作 CS */
    while (__HAL_SPI_GET_FLAG(ctx->hspi, SPI_FLAG_BSY)) {}
    return (st == HAL_OK) ? 0 : -1;
}

int32_t pl_spi_receive(pl_spi_handle_t h, uint8_t *data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;
    return (HAL_SPI_Receive(ctx->hspi, data, size, 100) == HAL_OK) ? 0 : -1;
}

int32_t pl_spi_receive_dma(pl_spi_handle_t h, uint8_t *data, uint16_t size)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return -1;

    /* DMA 够不到 CCMRAM：放进去不会报错，只是收到的数据不对，而且往往很久以后
       才被发现。这里当场拦下 —— 见 pl_mem.h。 */
    if (!pl_mem_is_dma_capable(data, size)) {
        printf("[pl_spi] DMA 目标落在 CCMRAM，DMA 不可达（buf=%p len=%u）\n", (void *)data,
               (unsigned)size);
        _record_failure(ctx, 0, size);
        return -1;
    }

    const uint32_t lock_before = (uint32_t)ctx->hspi->Lock;
    const HAL_StatusTypeDef st = HAL_SPI_Receive_DMA(ctx->hspi, data, size);
    if (st != HAL_OK)
        _record_failure(ctx, lock_before, size);
    return (st == HAL_OK) ? 0 : -1;
}

void pl_spi_abort(pl_spi_handle_t h)
{
    spi_ctx_t *ctx = (spi_ctx_t *)h;
    if (!ctx || !ctx->hspi) return;

    /* HAL_SPI_Abort 中止两条 DMA、清回调，正常时会把 State 落回 READY；
       但它自己也可能失败（流没使能 / 传输早已结束），失败路径会把状态留在
       BUSY_ABORT 之类的位置 —— 那之后所有 HAL_SPI_* 一律返回 HAL_BUSY。
       所以这里无条件把状态机拉回 READY（调用方是在"超时/启动失败"后收尾，
       此刻不存在还在进行的正常传输）。 */
    (void)HAL_SPI_Abort(ctx->hspi);
    ctx->hspi->State = HAL_SPI_STATE_READY;

    /* 半双工读时把 SPI 外设恢复到使能（HAL_SPI_Abort 会关掉 SPE） */
    if ((ctx->hspi->Instance->CR1 & SPI_CR1_SPE) == 0U) {
        __HAL_SPI_ENABLE(ctx->hspi);
    }
}

/* SPI DMA 完成回调 → 转发到 Device 层 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1 && s_spi_ctx.rx_done_fn)
        s_spi_ctx.rx_done_fn(s_spi_ctx.rx_done_ctx);
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1 && s_spi_ctx.rx_done_fn)
        s_spi_ctx.rx_done_fn(s_spi_ctx.rx_done_ctx);
}

/* ---- SPI ISR (从 stm32f4xx_it.c 迁移) ---- */
void SPI1_IRQHandler(void) { HAL_SPI_IRQHandler(&hspi1); }
