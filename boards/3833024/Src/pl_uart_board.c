/**
 * @file    pl_uart_board.c
 * @brief   3833024 的 UART 板级表与中断向量
 *
 * 3833024 三路 UART：
 *   USART1 → RS485       ，RX 走 DMA2_Stream2
 *   USART3 → RS232-0     ，RX 走 DMA1_Stream1
 *   USART6 → RS232-1     ，RX 走 DMA2_Stream1
 *
 * "有哪些中断向量、哪个 DMA 流配哪一路"是板级事实，所以向量写在这里，
 * 函数体复用共享的 pl_uart_irq_handler / pl_uart_dma_irq_handler。
 */

#include "pl_uart.h"
#include "usart.h"
#include "dma.h"

/* UART 的 DMA 接收句柄定义在 usart.c（CubeMX 把 UART 的 DMA 配置放在那里，
   不在 dma.c），usart.h 也不声明它们——旧 pl_uart.c 是就地 extern 的，
   现在归板级文件声明。 */
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern DMA_HandleTypeDef hdma_usart6_rx;

const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX] = {
    [PL_UART1] = {.init = MX_USART1_UART_Init, .huart = &huart1, .dma_rx = &hdma_usart1_rx, .irq = USART1_IRQn, .dma_irq = DMA2_Stream2_IRQn},
    [PL_UART3] = {.init = MX_USART3_UART_Init, .huart = &huart3, .dma_rx = &hdma_usart3_rx, .irq = USART3_IRQn, .dma_irq = DMA1_Stream1_IRQn},
    [PL_UART6] = {.init = MX_USART6_UART_Init, .huart = &huart6, .dma_rx = &hdma_usart6_rx, .irq = USART6_IRQn, .dma_irq = DMA2_Stream1_IRQn},
};

/* ---- 中断向量 ---- */

void USART1_IRQHandler(void)
{
    pl_uart_irq_handler(PL_UART1);
}
void USART3_IRQHandler(void)
{
    pl_uart_irq_handler(PL_UART3);
}
void USART6_IRQHandler(void)
{
    pl_uart_irq_handler(PL_UART6);
}

void DMA1_Stream1_IRQHandler(void)
{
    pl_uart_dma_irq_handler(PL_UART3);
}
void DMA2_Stream1_IRQHandler(void)
{
    pl_uart_dma_irq_handler(PL_UART6);
}
void DMA2_Stream2_IRQHandler(void)
{
    pl_uart_dma_irq_handler(PL_UART1);
}
