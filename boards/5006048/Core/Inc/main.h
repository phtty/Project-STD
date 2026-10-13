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
#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

// SRAM存储地址映射
#define BITBAND_SRAM(address, bit) (*(volatile uint32_t *)(0x22000000 + ((uint32_t)(address) - 0x20000000) * 0x20 + (bit) * 0x04))
// 外设地址映射
#define BITBAND_PERIPH(address, bit) (*(volatile uint32_t *)(0x42000000 + ((uint32_t)(address) - 0x40000000) * 0x20 + (bit) * 0x04))

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* ---- BarBoard 屏体引脚 (自 B 工程移植) ---- */
/* LwIP 调试开关 — 默认关闭。
   注意不能只改成 LWIP_DBG_OFF：#ifdef LWIP_DEBUG 仍成立，调试代码与格式串
   照样编入固件。必须整行注释掉，LWIP_DEBUGF 才会退化为空操作。
   排查网络问题时临时打开其中一行即可。
#define LWIP_DEBUG            LWIP_DBG_ON
#define SOCKETS_DEBUG         LWIP_DBG_ON
#define TCP_DEBUG             LWIP_DBG_ON
#define LWIP_DBG_MIN_LEVEL    LWIP_DBG_LEVEL_ALL
*/
/* HUB75 数据通道 LED_CH0..59 (第 6 通道线各接口未用, 保留定义以对齐 B 配置) */
#define LED_CH0_Pin           GPIO_PIN_6
#define LED_CH0_GPIO_Port     GPIOA
#define LED_CH1_Pin           GPIO_PIN_4
#define LED_CH1_GPIO_Port     GPIOA
#define LED_CH2_Pin           GPIO_PIN_3
#define LED_CH2_GPIO_Port     GPIOA
#define LED_CH3_Pin           GPIO_PIN_3
#define LED_CH3_GPIO_Port     GPIOC
#define LED_CH4_Pin           GPIO_PIN_2
#define LED_CH4_GPIO_Port     GPIOC
#define LED_CH5_Pin           GPIO_PIN_0
#define LED_CH5_GPIO_Port     GPIOC
#define LED_CH6_Pin           GPIO_PIN_8
#define LED_CH6_GPIO_Port     GPIOF
#define LED_CH7_Pin           GPIO_PIN_7
#define LED_CH7_GPIO_Port     GPIOF
#define LED_CH8_Pin           GPIO_PIN_6
#define LED_CH8_GPIO_Port     GPIOF
#define LED_CH9_Pin           GPIO_PIN_5
#define LED_CH9_GPIO_Port     GPIOF
#define LED_CH10_Pin          GPIO_PIN_4
#define LED_CH10_GPIO_Port    GPIOF
#define LED_CH11_Pin          GPIO_PIN_3
#define LED_CH11_GPIO_Port    GPIOF
#define LED_CH12_Pin          GPIO_PIN_2
#define LED_CH12_GPIO_Port    GPIOF
#define LED_CH13_Pin          GPIO_PIN_1
#define LED_CH13_GPIO_Port    GPIOF
#define LED_CH14_Pin          GPIO_PIN_0
#define LED_CH14_GPIO_Port    GPIOF
#define LED_CH15_Pin          GPIO_PIN_15
#define LED_CH15_GPIO_Port    GPIOC
#define LED_CH16_Pin          GPIO_PIN_14
#define LED_CH16_GPIO_Port    GPIOC
#define LED_CH17_Pin          GPIO_PIN_13
#define LED_CH17_GPIO_Port    GPIOC
#define LED_CH18_Pin          GPIO_PIN_6
#define LED_CH18_GPIO_Port    GPIOE
#define LED_CH19_Pin          GPIO_PIN_5
#define LED_CH19_GPIO_Port    GPIOE
#define LED_CH20_Pin          GPIO_PIN_4
#define LED_CH20_GPIO_Port    GPIOE
#define LED_CH21_Pin          GPIO_PIN_3
#define LED_CH21_GPIO_Port    GPIOE
#define LED_CH22_Pin          GPIO_PIN_2
#define LED_CH22_GPIO_Port    GPIOE
#define LED_CH23_Pin          GPIO_PIN_1
#define LED_CH23_GPIO_Port    GPIOE
#define LED_CH24_Pin          GPIO_PIN_7
#define LED_CH24_GPIO_Port    GPIOD
#define LED_CH25_Pin          GPIO_PIN_6
#define LED_CH25_GPIO_Port    GPIOD
#define LED_CH26_Pin          GPIO_PIN_5
#define LED_CH26_GPIO_Port    GPIOD
#define LED_CH27_Pin          GPIO_PIN_4
#define LED_CH27_GPIO_Port    GPIOD
#define LED_CH28_Pin          GPIO_PIN_3
#define LED_CH28_GPIO_Port    GPIOD
#define LED_CH29_Pin          GPIO_PIN_2
#define LED_CH29_GPIO_Port    GPIOD
#define LED_CH30_Pin          GPIO_PIN_1
#define LED_CH30_GPIO_Port    GPIOD
#define LED_CH31_Pin          GPIO_PIN_0
#define LED_CH31_GPIO_Port    GPIOD
#define LED_CH32_Pin          GPIO_PIN_12
#define LED_CH32_GPIO_Port    GPIOC
#define LED_CH33_Pin          GPIO_PIN_12
#define LED_CH33_GPIO_Port    GPIOA
#define LED_CH34_Pin          GPIO_PIN_11
#define LED_CH34_GPIO_Port    GPIOA
#define LED_CH35_Pin          GPIO_PIN_9
#define LED_CH35_GPIO_Port    GPIOC
#define LED_CH36_Pin          GPIO_PIN_8
#define LED_CH36_GPIO_Port    GPIOC
#define LED_CH37_Pin          GPIO_PIN_8
#define LED_CH37_GPIO_Port    GPIOG
#define LED_CH38_Pin          GPIO_PIN_7
#define LED_CH38_GPIO_Port    GPIOG
#define LED_CH39_Pin          GPIO_PIN_6
#define LED_CH39_GPIO_Port    GPIOG
#define LED_CH40_Pin          GPIO_PIN_4
#define LED_CH40_GPIO_Port    GPIOG
#define LED_CH41_Pin          GPIO_PIN_3
#define LED_CH41_GPIO_Port    GPIOG
#define LED_CH42_Pin          GPIO_PIN_2
#define LED_CH42_GPIO_Port    GPIOG
#define LED_CH43_Pin          GPIO_PIN_15
#define LED_CH43_GPIO_Port    GPIOD
#define LED_CH44_Pin          GPIO_PIN_14
#define LED_CH44_GPIO_Port    GPIOD
#define LED_CH45_Pin          GPIO_PIN_13
#define LED_CH45_GPIO_Port    GPIOD
#define LED_CH46_Pin          GPIO_PIN_12
#define LED_CH46_GPIO_Port    GPIOD
#define LED_CH47_Pin          GPIO_PIN_11
#define LED_CH47_GPIO_Port    GPIOD
#define LED_CH48_Pin          GPIO_PIN_15
#define LED_CH48_GPIO_Port    GPIOB
#define LED_CH49_Pin          GPIO_PIN_14
#define LED_CH49_GPIO_Port    GPIOB
#define LED_CH50_Pin          GPIO_PIN_13
#define LED_CH50_GPIO_Port    GPIOB
#define LED_CH51_Pin          GPIO_PIN_12
#define LED_CH51_GPIO_Port    GPIOB
#define LED_CH52_Pin          GPIO_PIN_11
#define LED_CH52_GPIO_Port    GPIOB
#define LED_CH53_Pin          GPIO_PIN_10
#define LED_CH53_GPIO_Port    GPIOB
#define LED_CH54_Pin          GPIO_PIN_15
#define LED_CH54_GPIO_Port    GPIOE
#define LED_CH55_Pin          GPIO_PIN_14
#define LED_CH55_GPIO_Port    GPIOE
#define LED_CH56_Pin          GPIO_PIN_13
#define LED_CH56_GPIO_Port    GPIOE
#define LED_CH57_Pin          GPIO_PIN_12
#define LED_CH57_GPIO_Port    GPIOE
#define LED_CH58_Pin          GPIO_PIN_11
#define LED_CH58_GPIO_Port    GPIOE
#define LED_CH59_Pin          GPIO_PIN_10
#define LED_CH59_GPIO_Port    GPIOE
/* HUB75 控制脚 (标签交叉: HUB75_A→LED_C, HUB75_B→LED_D, HUB75_C→LED_A, HUB75_D→LED_B) */
#define LED_A_Pin             GPIO_PIN_1
#define LED_A_GPIO_Port       GPIOG
#define LED_B_Pin             GPIO_PIN_0
#define LED_B_GPIO_Port       GPIOG
#define LED_C_Pin             GPIO_PIN_15
#define LED_C_GPIO_Port       GPIOF
#define LED_D_Pin             GPIO_PIN_14
#define LED_D_GPIO_Port       GPIOF
#define LED_OE_Pin            GPIO_PIN_9
#define LED_OE_GPIO_Port      GPIOE
#define LED_CLK_Pin           GPIO_PIN_8
#define LED_CLK_GPIO_Port     GPIOE
#define LED_LE_Pin            GPIO_PIN_7
#define LED_LE_GPIO_Port      GPIOE
/* 光敏 ADC 输入 */
#define LIGHT_Pin             GPIO_PIN_5
#define LIGHT_GPIO_Port       GPIOA
/* NOR Flash (W25Q256, SPI1 + 软 CS) */
#define W25QXX_CS_Pin         GPIO_PIN_1
#define W25QXX_CS_GPIO_Port   GPIOB
#define W25QXX_CLK_Pin        GPIO_PIN_3
#define W25QXX_CLK_GPIO_Port  GPIOB
#define W25QXX_MISO_Pin       GPIO_PIN_4
#define W25QXX_MISO_GPIO_Port GPIOB
#define W25QXX_MOSI_Pin       GPIO_PIN_5
#define W25QXX_MOSI_GPIO_Port GPIOB
/* RS485 (USART1 + DE 方向脚) */
#define RS485_RE_Pin          GPIO_PIN_8
#define RS485_RE_GPIO_Port    GPIOA
#define RS485_TX_Pin          GPIO_PIN_9
#define RS485_TX_GPIO_Port    GPIOA
#define RS485_RX_Pin          GPIO_PIN_10
#define RS485_RX_GPIO_Port    GPIOA
/* 测试键 (工厂测试) */
#define KEY_TST_Pin           GPIO_PIN_8
#define KEY_TST_GPIO_Port     GPIOD
#define KEY_TST_EXTI_IRQn     EXTI9_5_IRQn
/* 状态 LED */
#define LED_Pin               GPIO_PIN_9
#define LED_GPIO_Port         GPIOD
/* 网口 link 状态输入 (与 B 硬件对齐, 当前无代码读取) */
#define ETH_STATUS_Pin        GPIO_PIN_5
#define ETH_STATUS_GPIO_Port  GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
