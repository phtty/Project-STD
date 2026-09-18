/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    gpio.c
 * @brief   This file provides code for the configuration
 *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
 * Analog
 * Input
 * Output
 * EVENT_OUT
 * EXTI
 */
/* 引脚配置自 B 工程 (BarBoard) 移植: 10 接口 P10 屏 + RS485 + W25Q256 + 测试键 */
void MX_GPIO_Init(void)
{

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOE, LED_CH22_Pin | LED_CH21_Pin | LED_CH20_Pin | LED_CH19_Pin
                          | LED_CH18_Pin | LED_LE_Pin | LED_CLK_Pin | LED_OE_Pin
                          | LED_CH59_Pin | LED_CH58_Pin | LED_CH57_Pin | LED_CH56_Pin
                          | LED_CH55_Pin | LED_CH54_Pin | LED_CH23_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOC, LED_CH17_Pin | LED_CH16_Pin | LED_CH15_Pin | LED_CH5_Pin
                          | LED_CH4_Pin | LED_CH3_Pin | LED_CH36_Pin | LED_CH35_Pin
                          | LED_CH32_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOF, LED_CH14_Pin | LED_CH13_Pin | LED_CH12_Pin | LED_CH11_Pin
                          | LED_CH10_Pin | LED_CH9_Pin | LED_CH8_Pin | LED_CH7_Pin
                          | LED_CH6_Pin | LED_D_Pin | LED_C_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOA, LED_CH2_Pin | LED_CH1_Pin | LED_CH0_Pin | RS485_RE_Pin
                          | LED_CH34_Pin | LED_CH33_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOB, W25QXX_CS_Pin | LED_CH53_Pin | LED_CH52_Pin | LED_CH51_Pin
                          | LED_CH50_Pin | LED_CH49_Pin | LED_CH48_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOG, LED_B_Pin | LED_A_Pin | LED_CH42_Pin | LED_CH41_Pin
                          | LED_CH40_Pin | LED_CH39_Pin | LED_CH38_Pin | LED_CH37_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOD, LED_Pin | LED_CH47_Pin | LED_CH46_Pin | LED_CH45_Pin
                          | LED_CH44_Pin | LED_CH43_Pin | LED_CH31_Pin | LED_CH30_Pin
                          | LED_CH29_Pin | LED_CH28_Pin | LED_CH27_Pin | LED_CH26_Pin
                          | LED_CH25_Pin | LED_CH24_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pins : LED_CH22_Pin LED_CH21_Pin LED_CH20_Pin LED_CH19_Pin
                           LED_CH18_Pin LED_LE_Pin LED_CLK_Pin LED_OE_Pin
                           LED_CH59_Pin LED_CH58_Pin LED_CH57_Pin LED_CH56_Pin
                           LED_CH55_Pin LED_CH54_Pin LED_CH23_Pin */
    GPIO_InitStruct.Pin   = LED_CH22_Pin | LED_CH21_Pin | LED_CH20_Pin | LED_CH19_Pin
                          | LED_CH18_Pin | LED_LE_Pin | LED_CLK_Pin | LED_OE_Pin
                          | LED_CH59_Pin | LED_CH58_Pin | LED_CH57_Pin | LED_CH56_Pin
                          | LED_CH55_Pin | LED_CH54_Pin | LED_CH23_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_CH17_Pin LED_CH16_Pin LED_CH15_Pin LED_CH5_Pin
                           LED_CH4_Pin LED_CH3_Pin LED_CH36_Pin LED_CH35_Pin
                           LED_CH32_Pin */
    GPIO_InitStruct.Pin   = LED_CH17_Pin | LED_CH16_Pin | LED_CH15_Pin | LED_CH5_Pin
                          | LED_CH4_Pin | LED_CH3_Pin | LED_CH36_Pin | LED_CH35_Pin
                          | LED_CH32_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_CH14_Pin LED_CH13_Pin LED_CH12_Pin LED_CH11_Pin
                           LED_CH10_Pin LED_CH9_Pin LED_CH8_Pin LED_CH7_Pin
                           LED_CH6_Pin LED_D_Pin LED_C_Pin */
    GPIO_InitStruct.Pin   = LED_CH14_Pin | LED_CH13_Pin | LED_CH12_Pin | LED_CH11_Pin
                          | LED_CH10_Pin | LED_CH9_Pin | LED_CH8_Pin | LED_CH7_Pin
                          | LED_CH6_Pin | LED_D_Pin | LED_C_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_CH2_Pin LED_CH1_Pin LED_CH0_Pin RS485_RE_Pin
                           LED_CH34_Pin LED_CH33_Pin */
    GPIO_InitStruct.Pin   = LED_CH2_Pin | LED_CH1_Pin | LED_CH0_Pin | RS485_RE_Pin
                          | LED_CH34_Pin | LED_CH33_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /*Configure GPIO pins : W25QXX_CS_Pin LED_CH53_Pin LED_CH52_Pin LED_CH51_Pin
                           LED_CH50_Pin LED_CH49_Pin LED_CH48_Pin */
    GPIO_InitStruct.Pin   = W25QXX_CS_Pin | LED_CH53_Pin | LED_CH52_Pin | LED_CH51_Pin
                          | LED_CH50_Pin | LED_CH49_Pin | LED_CH48_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_B_Pin LED_A_Pin LED_CH42_Pin LED_CH41_Pin
                           LED_CH40_Pin LED_CH39_Pin LED_CH38_Pin LED_CH37_Pin */
    GPIO_InitStruct.Pin   = LED_B_Pin | LED_A_Pin | LED_CH42_Pin | LED_CH41_Pin
                          | LED_CH40_Pin | LED_CH39_Pin | LED_CH38_Pin | LED_CH37_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    /*Configure GPIO pin : KEY_TST_Pin */
    GPIO_InitStruct.Pin  = KEY_TST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KEY_TST_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_Pin LED_CH47_Pin LED_CH46_Pin LED_CH45_Pin
                           LED_CH44_Pin LED_CH43_Pin LED_CH31_Pin LED_CH30_Pin
                           LED_CH29_Pin LED_CH28_Pin LED_CH27_Pin LED_CH26_Pin
                           LED_CH25_Pin LED_CH24_Pin */
    GPIO_InitStruct.Pin   = LED_Pin | LED_CH47_Pin | LED_CH46_Pin | LED_CH45_Pin
                          | LED_CH44_Pin | LED_CH43_Pin | LED_CH31_Pin | LED_CH30_Pin
                          | LED_CH29_Pin | LED_CH28_Pin | LED_CH27_Pin | LED_CH26_Pin
                          | LED_CH25_Pin | LED_CH24_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /*Configure GPIO pin : ETH_STATUS_Pin */
    GPIO_InitStruct.Pin  = ETH_STATUS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ETH_STATUS_GPIO_Port, &GPIO_InitStruct);

    /* EXTI interrupt init*/
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
