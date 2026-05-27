/**
 * @file        pl_dma.c
 * @brief       DMA 初始化（hw_subsys_initcall 优先级 2）
 */

#include "pl_dma.h"
#include "dma.h"
#include "initcall.h"

void pl_dma_init(void)
{
    MX_DMA_Init();
}
hw_subsys_initcall(pl_dma_init);

/* ---- DMA SPI ISR (从 stm32f4xx_it.c 迁移) ---- */
extern DMA_HandleTypeDef hdma_spi1_rx;
extern DMA_HandleTypeDef hdma_spi1_tx;
void DMA2_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_rx); }
void DMA2_Stream3_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_tx); }
