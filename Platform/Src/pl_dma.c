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
