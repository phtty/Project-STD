/**
 * @file        pl_uart.c
 * @brief       UART 平台层抽象（hw_pl_initcall 优先级 3）
 *
 * 提供不透明句柄、阻塞发送和 DMA 空闲中断接收。
 *
 * 本文件只有机制，不含"本板有哪几路 UART"——那来自板级的 g_pl_uart_board[]
 * （见 boards/<板>/Src/pl_uart_board.c）。ISR 向量也在板级文件里，因为
 * "有哪些中断向量、哪个 DMA 流配哪一路"同样是板级事实。
 */

#include "pl_uart.h"
#include "initcall.h"
#include "pl_mem.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    UART_HandleTypeDef *huart;
    pl_uart_rx_cb_t rx_cb;
    void *rx_cb_ctx;
    pl_uart_dir_fn_t dir_cb;   /* Device 层注入的 RS485 方向控制，无则为 NULL */
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
} uart_ctx_t;

static uart_ctx_t g_uart_ctx[PL_UART_MAX];

/* ---- 内部：UART 空闲中断处理 ---- */
static void uart_idle_handle(uart_ctx_t *ctx)
{
    if (!(__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE))) return;
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    HAL_UART_DMAStop(ctx->huart);

    uint16_t len = ctx->rx_buf_size - __HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);

    if (ctx->rx_cb && len > 0)
        ctx->rx_cb(ctx->rx_buf, len, ctx->rx_cb_ctx);

    HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, ctx->rx_buf_size);
    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
}

/* ---- initcall ---- */
void pl_uart_init(void)
{
    for (uint8_t i = 0; i < PL_UART_MAX; i++) {
        if (g_pl_uart_board[i].init) g_pl_uart_board[i].init();
        g_uart_ctx[i].huart = (UART_HandleTypeDef *)g_pl_uart_board[i].huart;
    }
}
hw_pl_initcall(pl_uart_init); /* 优先级 2: 在 device 驱动之前 */

/* ---- 公开 API ---- */
pl_uart_handle_t pl_uart_get_handle(uint8_t id)
{
    return (id < PL_UART_MAX) ? &g_uart_ctx[id] : NULL;
}

int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (!ctx || !ctx->huart) return -1;

    if (ctx->dir_cb) ctx->dir_cb(true);
    HAL_StatusTypeDef st = HAL_UART_Transmit(ctx->huart, (uint8_t *)buf, len, timeout_ms);
    if (ctx->dir_cb) ctx->dir_cb(false);
    return (st == HAL_OK) ? (int32_t)len : -1;
}

void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx_arg)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (ctx) {
        ctx->rx_cb     = cb;
        ctx->rx_cb_ctx = ctx_arg;
    }
}

void pl_uart_set_dir_cb(pl_uart_handle_t h, pl_uart_dir_fn_t cb)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (ctx) ctx->dir_cb = cb;
}

int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (!ctx || !ctx->huart) return -1;

    /* DMA 够不到 CCMRAM（见 pl_mem.h）：放进去不会报错，只是收到的数据不对。
       这里当场拦下。 */
    if (!pl_mem_is_dma_capable(buf, len)) {
        printf("[pl_uart] DMA 接收缓冲落在 CCMRAM，DMA 不可达（buf=%p len=%u）\n",
               (void *)buf, (unsigned)len);
        return -1;
    }

    ctx->rx_buf      = buf;
    ctx->rx_buf_size = len;

    HAL_StatusTypeDef st = HAL_UART_Receive_DMA(ctx->huart, buf, len);
    if (st != HAL_OK) return -1;

    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
    return 0;
}

/* ================================================================
 *  中断入口（由 boards/<板>/Src/pl_uart_board.c 里的向量调用）
 * ================================================================ */

void pl_uart_irq_handler(uint8_t id)
{
    if (id >= PL_UART_MAX || !g_uart_ctx[id].huart) return;
    HAL_UART_IRQHandler(g_uart_ctx[id].huart);
    uart_idle_handle(&g_uart_ctx[id]);
}

void pl_uart_dma_irq_handler(uint8_t id)
{
    if (id >= PL_UART_MAX) return;
    DMA_HandleTypeDef *hdma = (DMA_HandleTypeDef *)g_pl_uart_board[id].dma_rx;
    if (hdma) HAL_DMA_IRQHandler(hdma);
}
