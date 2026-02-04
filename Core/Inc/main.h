/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.h
 * @brief          : Header for main.c file.
 *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

// SRAM区地址位带映射
#define BITBAND_SRAM(address, bit) (*(volatile uint32_t *)(0x22000000 + ((uint32_t)(address) - 0x20000000) * 0x20 + (bit) * 0x04))
// 外设区地址位带映射
#define BITBAND_PERIPH(address, bit) (*(volatile uint32_t *)(0x42000000 + ((uint32_t)(address) - 0x40000000) * 0x20 + (bit) * 0x04))

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LWIP_DEBUG            LWIP_DBG_ON
#define SOCKETS_DEBUG         LWIP_DBG_ON
#define TCP_DEBUG             LWIP_DBG_ON
#define LWIP_DBG_MIN_LEVEL    LWIP_DBG_LEVEL_ALL
#define HUB75_G4_Pin          GPIO_PIN_2
#define HUB75_G4_GPIO_Port    GPIOE
#define HUB75_B4_Pin          GPIO_PIN_3
#define HUB75_B4_GPIO_Port    GPIOE
#define HUB75_R5_Pin          GPIO_PIN_4
#define HUB75_R5_GPIO_Port    GPIOE
#define HUB75_G5_Pin          GPIO_PIN_5
#define HUB75_G5_GPIO_Port    GPIOE
#define HUB75_B5_Pin          GPIO_PIN_6
#define HUB75_B5_GPIO_Port    GPIOE
#define HUB75_R6_Pin          GPIO_PIN_13
#define HUB75_R6_GPIO_Port    GPIOC
#define HUB75_G6_Pin          GPIO_PIN_14
#define HUB75_G6_GPIO_Port    GPIOC
#define HUB75_B6_Pin          GPIO_PIN_15
#define HUB75_B6_GPIO_Port    GPIOC
#define HUB75_R7_Pin          GPIO_PIN_0
#define HUB75_R7_GPIO_Port    GPIOF
#define HUB75_G7_Pin          GPIO_PIN_1
#define HUB75_G7_GPIO_Port    GPIOF
#define HUB75_B7_Pin          GPIO_PIN_2
#define HUB75_B7_GPIO_Port    GPIOF
#define HUB75_R8_Pin          GPIO_PIN_3
#define HUB75_R8_GPIO_Port    GPIOF
#define HUB75_G8_Pin          GPIO_PIN_4
#define HUB75_G8_GPIO_Port    GPIOF
#define HUB75_B8_Pin          GPIO_PIN_5
#define HUB75_B8_GPIO_Port    GPIOF
#define HUB75_R9_Pin          GPIO_PIN_6
#define HUB75_R9_GPIO_Port    GPIOF
#define HUB75_G9_Pin          GPIO_PIN_7
#define HUB75_G9_GPIO_Port    GPIOF
#define HUB75_B9_Pin          GPIO_PIN_8
#define HUB75_B9_GPIO_Port    GPIOF
#define HUB75_R10_Pin         GPIO_PIN_9
#define HUB75_R10_GPIO_Port   GPIOF
#define HUB75_G10_Pin         GPIO_PIN_10
#define HUB75_G10_GPIO_Port   GPIOF
#define HUB75_B10_Pin         GPIO_PIN_0
#define HUB75_B10_GPIO_Port   GPIOC
#define LDR_Pin               GPIO_PIN_5
#define LDR_GPIO_Port         GPIOA
#define W25QXX_CS_Pin         GPIO_PIN_1
#define W25QXX_CS_GPIO_Port   GPIOB
#define DIP_SW1_Pin           GPIO_PIN_7
#define DIP_SW1_GPIO_Port     GPIOE
#define DIP_SW2_Pin           GPIO_PIN_8
#define DIP_SW2_GPIO_Port     GPIOE
#define SW3_Pin               GPIO_PIN_10
#define SW3_GPIO_Port         GPIOE
#define SW3_EXTI_IRQn         EXTI15_10_IRQn
#define SW2_Pin               GPIO_PIN_11
#define SW2_GPIO_Port         GPIOE
#define SW2_EXTI_IRQn         EXTI15_10_IRQn
#define SW1_Pin               GPIO_PIN_12
#define SW1_GPIO_Port         GPIOE
#define SW1_EXTI_IRQn         EXTI15_10_IRQn
#define RS232_TX1_Pin         GPIO_PIN_10
#define RS232_TX1_GPIO_Port   GPIOB
#define RS232_RX1_Pin         GPIO_PIN_11
#define RS232_RX1_GPIO_Port   GPIOB
#define KEY_TST_Pin           GPIO_PIN_8
#define KEY_TST_GPIO_Port     GPIOD
#define KEY_TST_EXTI_IRQn     EXTI9_5_IRQn
#define LED_Pin               GPIO_PIN_9
#define LED_GPIO_Port         GPIOD
#define Lane_Light_Pin        GPIO_PIN_14
#define Lane_Light_GPIO_Port  GPIOD
#define Flash_Light_Pin       GPIO_PIN_15
#define Flash_Light_GPIO_Port GPIOD
#define RS232_TX2_Pin         GPIO_PIN_6
#define RS232_TX2_GPIO_Port   GPIOC
#define RS232_RX2_Pin         GPIO_PIN_7
#define RS232_RX2_GPIO_Port   GPIOC
#define RS485_RE_Pin          GPIO_PIN_8
#define RS485_RE_GPIO_Port    GPIOA
#define RS485_TX_Pin          GPIO_PIN_9
#define RS485_TX_GPIO_Port    GPIOA
#define RS485_RX_Pin          GPIO_PIN_10
#define RS485_RX_GPIO_Port    GPIOA
#define HUB75_OE_Pin          GPIO_PIN_0
#define HUB75_OE_GPIO_Port    GPIOD
#define HUB75_CLK_Pin         GPIO_PIN_1
#define HUB75_CLK_GPIO_Port   GPIOD
#define HUB75_LAT_Pin         GPIO_PIN_3
#define HUB75_LAT_GPIO_Port   GPIOD
#define HUB75_A_Pin           GPIO_PIN_4
#define HUB75_A_GPIO_Port     GPIOD
#define HUB75_B_Pin           GPIO_PIN_5
#define HUB75_B_GPIO_Port     GPIOD
#define HUB75_C_Pin           GPIO_PIN_6
#define HUB75_C_GPIO_Port     GPIOD
#define HUB75_D_Pin           GPIO_PIN_7
#define HUB75_D_GPIO_Port     GPIOD
#define HUB75_R1_Pin          GPIO_PIN_9
#define HUB75_R1_GPIO_Port    GPIOG
#define HUB75_G1_Pin          GPIO_PIN_10
#define HUB75_G1_GPIO_Port    GPIOG
#define HUB75_B1_Pin          GPIO_PIN_12
#define HUB75_B1_GPIO_Port    GPIOG
#define HUB75_R2_Pin          GPIO_PIN_15
#define HUB75_R2_GPIO_Port    GPIOG
#define W25QXX_CLK_Pin        GPIO_PIN_3
#define W25QXX_CLK_GPIO_Port  GPIOB
#define W25QXX_MISO_Pin       GPIO_PIN_4
#define W25QXX_MISO_GPIO_Port GPIOB
#define W25QXX_MOSI_Pin       GPIO_PIN_5
#define W25QXX_MOSI_GPIO_Port GPIOB
#define HUB75_G2_Pin          GPIO_PIN_6
#define HUB75_G2_GPIO_Port    GPIOB
#define HUB75_B2_Pin          GPIO_PIN_7
#define HUB75_B2_GPIO_Port    GPIOB
#define HUB75_R3_Pin          GPIO_PIN_8
#define HUB75_R3_GPIO_Port    GPIOB
#define HUB75_G3_Pin          GPIO_PIN_9
#define HUB75_G3_GPIO_Port    GPIOB
#define HUB75_B3_Pin          GPIO_PIN_0
#define HUB75_B3_GPIO_Port    GPIOE
#define HUB75_R4_Pin          GPIO_PIN_1
#define HUB75_R4_GPIO_Port    GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
