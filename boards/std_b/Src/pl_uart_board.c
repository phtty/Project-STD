/**
 * @file    pl_uart_board.c
 * @brief   std_b 的 UART 板级表与中断向量
 *
 * std_b 只有一路 UART：USART1 → RS485，RX 走 DMA2_Stream2。
 * PL_UART3 / PL_UART6（std_a 的两路 RS232）在本板留空。
 */

#include "pl_uart.h"
#include "usart.h"

/* UART 的 DMA 接收句柄定义在 usart.c（CubeMX 把 UART 的 DMA 配置放在那里，
   不在 dma.c），usart.h 也不声明它们。 */
extern DMA_HandleTypeDef hdma_usart1_rx;

const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX] = {
    [PL_UART1] = {.init = MX_USART1_UART_Init, .huart = &huart1, .dma_rx = &hdma_usart1_rx, .irq = USART1_IRQn, .dma_irq = DMA2_Stream2_IRQn},
};

/* ---- 中断向量 ---- */

void USART1_IRQHandler(void)
{
    pl_uart_irq_handler(PL_UART1);
}

void DMA2_Stream2_IRQHandler(void)
{
    pl_uart_dma_irq_handler(PL_UART1);
}
