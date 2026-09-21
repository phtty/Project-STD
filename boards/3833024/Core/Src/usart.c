/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    usart.c
 * @brief   This file provides code for the configuration
 *          of the USART instances.
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "usart.h"

/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart6;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart3_rx;
DMA_HandleTypeDef hdma_usart6_rx;
/* TX 三条手写补充，非 CubeMX 产物（本工程不再重新生成 CubeMX 代码） */
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart3_tx;
DMA_HandleTypeDef hdma_usart6_tx;

/* USART1 init function */

void MX_USART1_UART_Init(void)
{

    /* USER CODE BEGIN USART1_Init 0 */

    /* USER CODE END USART1_Init 0 */

    /* USER CODE BEGIN USART1_Init 1 */

    /* USER CODE END USART1_Init 1 */
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
    /* USER CODE BEGIN USART1_Init 2 */

    /* USER CODE END USART1_Init 2 */
}
/* USART3 init function */

void MX_USART3_UART_Init(void)
{

    /* USER CODE BEGIN USART3_Init 0 */

    /* USER CODE END USART3_Init 0 */

    /* USER CODE BEGIN USART3_Init 1 */

    /* USER CODE END USART3_Init 1 */
    huart3.Instance          = USART3;
    huart3.Init.BaudRate     = 115200;
    huart3.Init.WordLength   = UART_WORDLENGTH_8B;
    huart3.Init.StopBits     = UART_STOPBITS_1;
    huart3.Init.Parity       = UART_PARITY_NONE;
    huart3.Init.Mode         = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK) {
        Error_Handler();
    }
    /* USER CODE BEGIN USART3_Init 2 */

    /* USER CODE END USART3_Init 2 */
}
/* USART6 init function */

void MX_USART6_UART_Init(void)
{

    /* USER CODE BEGIN USART6_Init 0 */

    /* USER CODE END USART6_Init 0 */

    /* USER CODE BEGIN USART6_Init 1 */

    /* USER CODE END USART6_Init 1 */
    huart6.Instance          = USART6;
    huart6.Init.BaudRate     = 115200;
    huart6.Init.WordLength   = UART_WORDLENGTH_8B;
    huart6.Init.StopBits     = UART_STOPBITS_1;
    huart6.Init.Parity       = UART_PARITY_NONE;
    huart6.Init.Mode         = UART_MODE_TX_RX;
    huart6.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart6) != HAL_OK) {
        Error_Handler();
    }
    /* USER CODE BEGIN USART6_Init 2 */
    /* USER CODE END USART6_Init 2 */
}

void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if (uartHandle->Instance == USART1) {
        /* USER CODE BEGIN USART1_MspInit 0 */

        /* USER CODE END USART1_MspInit 0 */
        /* USART1 clock enable */
        __HAL_RCC_USART1_CLK_ENABLE();

        __HAL_RCC_GPIOA_CLK_ENABLE();
        /**USART1 GPIO Configuration
        PA9     ------> USART1_TX
        PA10     ------> USART1_RX
        */
        GPIO_InitStruct.Pin       = RS485_TX_Pin | RS485_RX_Pin;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* USART1 DMA Init */
        /* USART1_RX Init */
        hdma_usart1_rx.Instance                 = DMA2_Stream2;
        hdma_usart1_rx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart1_rx.Init.Mode                = DMA_NORMAL;
        hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart1_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmarx, hdma_usart1_rx);

        /* USART1_TX Init —— **手写，非 CubeMX 产物**
           TX DMA 是长帧发送的前提：轮询会把 CPU 按在整帧的物理时间上，而扫描任务是 Realtime 的。
           选 DMA2_Stream7/Ch4；F407 上 USART1_TX 的可选流里这条空闲且不与本板
           已用的 S0(SPI1_RX)/S1(USART6_RX)/S2(USART1_RX)/S3(SPI1_TX)/DMA1_S1(USART3_RX) 冲突。 */
        hdma_usart1_tx.Instance                 = DMA2_Stream7;
        hdma_usart1_tx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_usart1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart1_tx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart1_tx.Init.Mode                = DMA_NORMAL;
        hdma_usart1_tx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart1_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmatx, hdma_usart1_tx);

        /* TX DMA 中断优先级取 7（与既有 DMA 一致）—— 必须 ≥
           configMAX_SYSCALL_INTERRUPT_PRIORITY(5)，因为 TX 完成回调里要 release 信号量 */
        HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);


        /* USART1 interrupt Init */
        HAL_NVIC_SetPriority(USART1_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
        /* USER CODE BEGIN USART1_MspInit 1 */

        /* USER CODE END USART1_MspInit 1 */
    } else if (uartHandle->Instance == USART3) {
        /* USER CODE BEGIN USART3_MspInit 0 */

        /* USER CODE END USART3_MspInit 0 */
        /* USART3 clock enable */
        __HAL_RCC_USART3_CLK_ENABLE();

        __HAL_RCC_GPIOB_CLK_ENABLE();
        /**USART3 GPIO Configuration
        PB10     ------> USART3_TX
        PB11     ------> USART3_RX
        */
        GPIO_InitStruct.Pin       = RS232_TX1_Pin | RS232_RX1_Pin;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* USART3 DMA Init */
        /* USART3_RX Init */
        hdma_usart3_rx.Instance                 = DMA1_Stream1;
        hdma_usart3_rx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart3_rx.Init.Mode                = DMA_NORMAL;
        hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmarx, hdma_usart3_rx);

        /* USART3_TX Init —— **手写，非 CubeMX 产物**
           同上，RS232-0 也补上与 RS485 一致的能力（IAP 在 RS232 上也有长帧）。
           选 DMA1_Stream3/Ch4；F407 上 USART3_TX 的可选流里这条空闲且不与本板
           已用的 S0(SPI1_RX)/S1(USART6_RX)/S2(USART1_RX)/S3(SPI1_TX)/DMA1_S1(USART3_RX) 冲突。 */
        hdma_usart3_tx.Instance                 = DMA1_Stream3;
        hdma_usart3_tx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart3_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_usart3_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart3_tx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart3_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart3_tx.Init.Mode                = DMA_NORMAL;
        hdma_usart3_tx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart3_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart3_tx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmatx, hdma_usart3_tx);

        /* TX DMA 中断优先级取 7（与既有 DMA 一致）—— 必须 ≥
           configMAX_SYSCALL_INTERRUPT_PRIORITY(5)，因为 TX 完成回调里要 release 信号量 */
        HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);


        /* USART3 interrupt Init */
        HAL_NVIC_SetPriority(USART3_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(USART3_IRQn);
        /* USER CODE BEGIN USART3_MspInit 1 */

        /* USER CODE END USART3_MspInit 1 */
    } else if (uartHandle->Instance == USART6) {
        /* USER CODE BEGIN USART6_MspInit 0 */

        /* USER CODE END USART6_MspInit 0 */
        /* USART6 clock enable */
        __HAL_RCC_USART6_CLK_ENABLE();

        __HAL_RCC_GPIOC_CLK_ENABLE();
        /**USART6 GPIO Configuration
        PC6     ------> USART6_TX
        PC7     ------> USART6_RX
        */
        GPIO_InitStruct.Pin       = RS232_TX2_Pin | RS232_RX2_Pin;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
        HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

        /* USART6 DMA Init */
        /* USART6_RX Init */
        hdma_usart6_rx.Instance                 = DMA2_Stream1;
        hdma_usart6_rx.Init.Channel             = DMA_CHANNEL_5;
        hdma_usart6_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        hdma_usart6_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart6_rx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart6_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart6_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart6_rx.Init.Mode                = DMA_NORMAL;
        hdma_usart6_rx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart6_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart6_rx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmarx, hdma_usart6_rx);

        /* USART6_TX Init —— **手写，非 CubeMX 产物**
           同上，RS232-1。
           选 DMA2_Stream6/Ch5；F407 上 USART6_TX 的可选流里这条空闲且不与本板
           已用的 S0(SPI1_RX)/S1(USART6_RX)/S2(USART1_RX)/S3(SPI1_TX)/DMA1_S1(USART3_RX) 冲突。 */
        hdma_usart6_tx.Instance                 = DMA2_Stream6;
        hdma_usart6_tx.Init.Channel             = DMA_CHANNEL_5;
        hdma_usart6_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_usart6_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart6_tx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart6_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart6_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart6_tx.Init.Mode                = DMA_NORMAL;
        hdma_usart6_tx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart6_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        if (HAL_DMA_Init(&hdma_usart6_tx) != HAL_OK) {
            Error_Handler();
        }

        __HAL_LINKDMA(uartHandle, hdmatx, hdma_usart6_tx);

        /* TX DMA 中断优先级取 7（与既有 DMA 一致）—— 必须 ≥
           configMAX_SYSCALL_INTERRUPT_PRIORITY(5)，因为 TX 完成回调里要 release 信号量 */
        HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);


        /* USART6 interrupt Init */
        HAL_NVIC_SetPriority(USART6_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(USART6_IRQn);
        /* USER CODE BEGIN USART6_MspInit 1 */

        /* USER CODE END USART6_MspInit 1 */
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *uartHandle)
{

    if (uartHandle->Instance == USART1) {
        /* USER CODE BEGIN USART1_MspDeInit 0 */

        /* USER CODE END USART1_MspDeInit 0 */
        /* Peripheral clock disable */
        __HAL_RCC_USART1_CLK_DISABLE();

        /**USART1 GPIO Configuration
        PA9     ------> USART1_TX
        PA10     ------> USART1_RX
        */
        HAL_GPIO_DeInit(GPIOA, RS485_TX_Pin | RS485_RX_Pin);

        /* USART1 DMA DeInit */
        HAL_DMA_DeInit(uartHandle->hdmarx);
        HAL_DMA_DeInit(uartHandle->hdmatx);

        /* USART1 interrupt Deinit */
        HAL_NVIC_DisableIRQ(USART1_IRQn);
        /* USER CODE BEGIN USART1_MspDeInit 1 */

        /* USER CODE END USART1_MspDeInit 1 */
    } else if (uartHandle->Instance == USART3) {
        /* USER CODE BEGIN USART3_MspDeInit 0 */

        /* USER CODE END USART3_MspDeInit 0 */
        /* Peripheral clock disable */
        __HAL_RCC_USART3_CLK_DISABLE();

        /**USART3 GPIO Configuration
        PB10     ------> USART3_TX
        PB11     ------> USART3_RX
        */
        HAL_GPIO_DeInit(GPIOB, RS232_TX1_Pin | RS232_RX1_Pin);

        /* USART3 DMA DeInit */
        HAL_DMA_DeInit(uartHandle->hdmarx);
        HAL_DMA_DeInit(uartHandle->hdmatx);

        /* USART3 interrupt Deinit */
        HAL_NVIC_DisableIRQ(USART3_IRQn);
        /* USER CODE BEGIN USART3_MspDeInit 1 */

        /* USER CODE END USART3_MspDeInit 1 */
    } else if (uartHandle->Instance == USART6) {
        /* USER CODE BEGIN USART6_MspDeInit 0 */

        /* USER CODE END USART6_MspDeInit 0 */
        /* Peripheral clock disable */
        __HAL_RCC_USART6_CLK_DISABLE();

        /**USART6 GPIO Configuration
        PC6     ------> USART6_TX
        PC7     ------> USART6_RX
        */
        HAL_GPIO_DeInit(GPIOC, RS232_TX2_Pin | RS232_RX2_Pin);

        /* USART6 DMA DeInit */
        HAL_DMA_DeInit(uartHandle->hdmarx);
        HAL_DMA_DeInit(uartHandle->hdmatx);

        /* USART6 interrupt Deinit */
        HAL_NVIC_DisableIRQ(USART6_IRQn);
        /* USER CODE BEGIN USART6_MspDeInit 1 */

        /* USER CODE END USART6_MspDeInit 1 */
    }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
