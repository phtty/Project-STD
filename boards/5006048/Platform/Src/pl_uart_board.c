/**
 * @file    pl_uart_board.c
 * @brief   5006048 的 UART 板级表与中断向量
 *
 * 5006048 只有一路 UART：USART1 → RS485，RX 走 DMA2_Stream2、TX 走 DMA2_Stream7。
 * PL_UART3 / PL_UART6（3833024 的两路 RS232）在本板留空。
 */

#include "pl_uart.h"
#include "usart.h"

/* UART 的 DMA 句柄定义在 usart.c（CubeMX 把 UART 的 DMA 配置放在那里，
   不在 dma.c），usart.h 也不声明它们。
   TX 那条是手写补进去的（本工程不再重新生成 CubeMX 代码），见 usart.c 的说明。 */
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;

const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX] = {
    [PL_UART1] = {.init = MX_USART1_UART_Init, .huart = &huart1, .dma_rx = &hdma_usart1_rx, .dma_tx = &hdma_usart1_tx, .irq = USART1_IRQn, .dma_irq = DMA2_Stream2_IRQn, .dma_tx_irq = DMA2_Stream7_IRQn},
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

void DMA2_Stream7_IRQHandler(void)
{
    pl_uart_dma_tx_irq_handler(PL_UART1);
}
