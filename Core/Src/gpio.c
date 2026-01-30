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
void MX_GPIO_Init(void)
{

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOE, HUB75_G4_Pin | HUB75_B4_Pin | HUB75_R5_Pin | HUB75_G5_Pin | HUB75_B5_Pin | HUB75_B3_Pin | HUB75_R4_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOC, HUB75_R6_Pin | HUB75_G6_Pin | HUB75_B6_Pin | HUB75_B10_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOF, HUB75_R7_Pin | HUB75_G7_Pin | HUB75_B7_Pin | HUB75_R8_Pin | HUB75_G8_Pin | HUB75_B8_Pin | HUB75_R9_Pin | HUB75_G9_Pin | HUB75_B9_Pin | HUB75_R10_Pin | HUB75_G10_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOB, W25QXX_CS_Pin | HUB75_G2_Pin | HUB75_B2_Pin | HUB75_R3_Pin | HUB75_G3_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOD, LED_Pin | Lane_Light_Pin | Flash_Light_Pin | HUB75_OE_Pin | HUB75_CLK_Pin | HUB75_LAT_Pin | HUB75_A_Pin | HUB75_B_Pin | HUB75_C_Pin | HUB75_D_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(RS485_RE_GPIO_Port, RS485_RE_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOG, HUB75_R1_Pin | HUB75_G1_Pin | HUB75_B1_Pin | HUB75_R2_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pins : HUB75_G4_Pin HUB75_B4_Pin HUB75_R5_Pin HUB75_G5_Pin
                             HUB75_B5_Pin HUB75_B3_Pin HUB75_R4_Pin */
    GPIO_InitStruct.Pin   = HUB75_G4_Pin | HUB75_B4_Pin | HUB75_R5_Pin | HUB75_G5_Pin | HUB75_B5_Pin | HUB75_B3_Pin | HUB75_R4_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /*Configure GPIO pins : HUB75_R6_Pin HUB75_G6_Pin HUB75_B6_Pin */
    GPIO_InitStruct.Pin   = HUB75_R6_Pin | HUB75_G6_Pin | HUB75_B6_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /*Configure GPIO pins : HUB75_R7_Pin HUB75_G7_Pin HUB75_B7_Pin HUB75_R8_Pin
                             HUB75_G8_Pin HUB75_B8_Pin HUB75_R9_Pin HUB75_G9_Pin
                             HUB75_B9_Pin HUB75_R10_Pin HUB75_G10_Pin */
    GPIO_InitStruct.Pin   = HUB75_R7_Pin | HUB75_G7_Pin | HUB75_B7_Pin | HUB75_R8_Pin | HUB75_G8_Pin | HUB75_B8_Pin | HUB75_R9_Pin | HUB75_G9_Pin | HUB75_B9_Pin | HUB75_R10_Pin | HUB75_G10_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

    /*Configure GPIO pin : HUB75_B10_Pin */
    GPIO_InitStruct.Pin   = HUB75_B10_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(HUB75_B10_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pin : W25QXX_CS_Pin */
    GPIO_InitStruct.Pin   = W25QXX_CS_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(W25QXX_CS_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pins : DIP_SW1_Pin DIP_SW2_Pin */
    GPIO_InitStruct.Pin  = DIP_SW1_Pin | DIP_SW2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /*Configure GPIO pins : SW3_Pin SW2_Pin SW1_Pin */
    GPIO_InitStruct.Pin  = SW3_Pin | SW2_Pin | SW1_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /*Configure GPIO pin : KEY_TST_Pin */
    GPIO_InitStruct.Pin  = KEY_TST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(KEY_TST_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pins : LED_Pin Lane_Light_Pin Flash_Light_Pin */
    GPIO_InitStruct.Pin   = LED_Pin | Lane_Light_Pin | Flash_Light_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /*Configure GPIO pin : RS485_RE_Pin */
    GPIO_InitStruct.Pin   = RS485_RE_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RS485_RE_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pins : HUB75_OE_Pin HUB75_CLK_Pin HUB75_LAT_Pin HUB75_A_Pin
                             HUB75_B_Pin HUB75_C_Pin HUB75_D_Pin */
    GPIO_InitStruct.Pin   = HUB75_OE_Pin | HUB75_CLK_Pin | HUB75_LAT_Pin | HUB75_A_Pin | HUB75_B_Pin | HUB75_C_Pin | HUB75_D_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /*Configure GPIO pins : HUB75_R1_Pin HUB75_G1_Pin HUB75_B1_Pin HUB75_R2_Pin */
    GPIO_InitStruct.Pin   = HUB75_R1_Pin | HUB75_G1_Pin | HUB75_B1_Pin | HUB75_R2_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    /*Configure GPIO pins : HUB75_G2_Pin HUB75_B2_Pin HUB75_R3_Pin HUB75_G3_Pin */
    GPIO_InitStruct.Pin   = HUB75_G2_Pin | HUB75_B2_Pin | HUB75_R3_Pin | HUB75_G3_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* EXTI interrupt init*/
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 8, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 8, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
