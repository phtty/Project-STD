/**
 ******************************************************************************
 * @file    stm32f4xx_it.c
 * @brief   Interrupt Service Routines.
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
#include "cmsis_os2.h"
/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/
extern osSemaphoreId_t test_semaphore;
extern osEventFlagsId_t SW123_Event;

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/

/* Private user code ---------------------------------------------------------*/

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
extern ADC_HandleTypeDef hadc1;
extern DMA_HandleTypeDef hdma_spi1_tx;
extern DMA_HandleTypeDef hdma_spi1_rx;
extern SPI_HandleTypeDef hspi1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4;
extern TIM_HandleTypeDef htim7;

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
 * @brief This function handles Non maskable interrupt.
 */
void NMI_Handler(void)
{

    while (1) {
    }
}

/**
 * @brief This function handles Hard fault interrupt.
 */
void HardFault_Handler(void)
{

    while (1) {
    }
}

/**
 * @brief This function handles Memory management fault.
 */
void MemManage_Handler(void)
{

    while (1) {
    }
}

/**
 * @brief This function handles Pre-fetch fault, memory access fault.
 */
void BusFault_Handler(void)
{

    while (1) {
    }
}

/**
 * @brief This function handles Undefined instruction or illegal state.
 */
void UsageFault_Handler(void)
{

    while (1) {
    }
}

/**
 * @brief This function handles Debug monitor.
 */
void DebugMon_Handler(void)
{

}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/* DMA1_Stream1 — 由 pl_uart.c 提供 */

/**
 * @brief This function handles ADC1, ADC2 and ADC3 global interrupts.
 */
void ADC_IRQHandler(void)
{

    HAL_ADC_IRQHandler(&hadc1);

}

/**
 * @brief This function handles EXTI line[9:5] interrupts.
 */
void EXTI9_5_IRQHandler(void)
{

    HAL_GPIO_EXTI_IRQHandler(KEY_TST_Pin);

}

/**
 * @brief This function handles TIM2 global interrupt.
 */
void TIM2_IRQHandler(void)
{

    HAL_TIM_IRQHandler(&htim2);

}

/**
 * @brief This function handles TIM3 global interrupt.
 */
void TIM3_IRQHandler(void)
{

    HAL_TIM_IRQHandler(&htim3);

}

/**
 * @brief This function handles TIM4 global interrupt.
 */
void TIM4_IRQHandler(void)
{

    HAL_TIM_IRQHandler(&htim4);

}

/**
 * @brief This function handles SPI1 global interrupt.
 */
void SPI1_IRQHandler(void)
{

    HAL_SPI_IRQHandler(&hspi1);

}

/* USART1/USART3 — 由 pl_uart.c 提供 */

/**
 * @brief This function handles EXTI line[15:10] interrupts.
 */
void EXTI15_10_IRQHandler(void)
{

    HAL_GPIO_EXTI_IRQHandler(SW3_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW2_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW1_Pin);

}

/**
 * @brief This function handles TIM7 global interrupt.
 */
void TIM7_IRQHandler(void)
{

    HAL_TIM_IRQHandler(&htim7);

}

/**
 * @brief This function handles DMA2 stream0 global interrupt.
 */
void DMA2_Stream0_IRQHandler(void)
{

    HAL_DMA_IRQHandler(&hdma_spi1_rx);

}

/* DMA2_Stream1/DMA2_Stream2 — 由 pl_uart.c 提供 */

/**
 * @brief This function handles DMA2 stream3 global interrupt.
 */
void DMA2_Stream3_IRQHandler(void)
{

    HAL_DMA_IRQHandler(&hdma_spi1_tx);

}

/**
 * @brief This function handles Ethernet global interrupt.
 */
void ETH_IRQHandler(void)
{

    HAL_ETH_IRQHandler(&heth);

}

/**
 * @brief This function handles Ethernet wake-up interrupt through EXTI line 19.
 */
void ETH_WKUP_IRQHandler(void)
{

    HAL_ETH_IRQHandler(&heth);

}

/* USART6 — 由 pl_uart.c 提供 */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY_TST_Pin) {
        osSemaphoreRelease(test_semaphore);
    }
    if (GPIO_Pin == SW1_Pin) {
        osEventFlagsSet(SW123_Event, SW1_EVENT);
    }
    if (GPIO_Pin == SW2_Pin) {
        osEventFlagsSet(SW123_Event, SW2_EVENT);
    }
    if (GPIO_Pin == SW3_Pin) {
        osEventFlagsSet(SW123_Event, SW3_EVENT);
    }
}
