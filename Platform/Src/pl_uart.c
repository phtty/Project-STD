/**
 * @file        pl_uart.c
 * @brief       UART 平台层抽象（hw_device_initcall 优先级 3）
 *
 * 封装 USART1 (RS485)、USART3 (RS232-0)、USART6 (RS232-1)，
 * 提供不透明句柄、阻塞发送和 DMA 空闲中断接收。
 * UART 及 DMA 中断处理全部内聚于此文件。
 */

#include "pl_uart.h"
#include "usart.h"
#include "dma.h"
#include "initcall.h"
#include <string.h>

/* DMA 句柄（定义在 Core/Src/dma.c） */
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern DMA_HandleTypeDef hdma_usart6_rx;

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
    MX_USART1_UART_Init();
    MX_USART3_UART_Init();
    MX_USART6_UART_Init();

    g_uart_ctx[PL_UART1].huart = &huart1;
    g_uart_ctx[PL_UART3].huart = &huart3;
    g_uart_ctx[PL_UART6].huart = &huart6;
}
hw_subsys_initcall(pl_uart_init); /* 优先级 2: 在 device 驱动之前 */

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

    ctx->rx_buf      = buf;
    ctx->rx_buf_size = len;

    HAL_StatusTypeDef st = HAL_UART_Receive_DMA(ctx->huart, buf, len);
    if (st != HAL_OK) return -1;

    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
    return 0;
}

/* ================================================================
 *  UART 及 DMA 中断服务例程（由 startup 向量表直接跳转）
 * ================================================================ */

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
    uart_idle_handle(&g_uart_ctx[PL_UART1]);
}

void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart3);
    uart_idle_handle(&g_uart_ctx[PL_UART3]);
}

void USART6_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart6);
    uart_idle_handle(&g_uart_ctx[PL_UART6]);
}

/* DMA RX 中断 */
void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_rx);
}
void DMA2_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart6_rx);
}
void DMA2_Stream2_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart1_rx);
}
