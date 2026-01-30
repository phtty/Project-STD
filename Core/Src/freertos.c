/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "tim.h"
#include "iwdg.h"
#include "rtc.h"
#include "SEGGER_RTT.h"
#include "w25qxx.h"
#include "render.h"
#include "tcp_sever.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
typedef StaticTask_t osStaticThreadDef_t;
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osThreadId_t HalfSecTaskHandle;
const osThreadAttr_t HalfSecTask_attributes = {
    .name       = "HalfSecTask",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

// osThreadId_t PointTestTaskHandle;
// const osThreadAttr_t PointTestTask_attributes = {
//     .name       = "PointTestTask",
//     .stack_size = 1024 * 4,
//     .priority   = (osPriority_t)osPriorityAboveNormal,
// };

/* USER CODE END Variables */
/* Definitions for InitTask */
osThreadId_t InitTaskHandle;
uint32_t InitTaskBuffer[256 * 4];
osStaticThreadDef_t InitTaskControlBlock;
const osThreadAttr_t InitTask_attributes = {
    .name       = "InitTask",
    .cb_mem     = &InitTaskControlBlock,
    .cb_size    = sizeof(InitTaskControlBlock),
    .stack_mem  = &InitTaskBuffer[0],
    .stack_size = sizeof(InitTaskBuffer),
    .priority   = (osPriority_t)osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void HalfSecTask(void *argument);
void PointTestTask(void *argument);
void RefreshTask(void *argument);
/* USER CODE END FunctionPrototypes */

void InitialTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
 * @brief  FreeRTOS initialization
 * @param  None
 * @retval None
 */
void MX_FREERTOS_Init(void)
{
    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* USER CODE BEGIN RTOS_MUTEX */
    /* add mutexes, ... */
    /* USER CODE END RTOS_MUTEX */

    /* USER CODE BEGIN RTOS_SEMAPHORES */
    /* add semaphores, ... */
    /* USER CODE END RTOS_SEMAPHORES */

    /* USER CODE BEGIN RTOS_TIMERS */
    /* start timers, add new ones, ... */
    /* USER CODE END RTOS_TIMERS */

    /* USER CODE BEGIN RTOS_QUEUES */
    /* add queues, ... */
    /* USER CODE END RTOS_QUEUES */

    /* Create the thread(s) */
    /* creation of InitTask */
    HalfSecTaskHandle = osThreadNew(HalfSecTask, NULL, &HalfSecTask_attributes);
    InitTaskHandle    = osThreadNew(InitialTask, NULL, &InitTask_attributes);

    /* USER CODE BEGIN RTOS_THREADS */
    /* add threads, ... */
    /* USER CODE END RTOS_THREADS */

    /* USER CODE BEGIN RTOS_EVENTS */
    /* add events, ... */
    /* USER CODE END RTOS_EVENTS */
}

/* USER CODE BEGIN Header_InitialTask */
/**
 * @brief  Function implementing the InitTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_InitialTask */
void InitialTask(void *argument)
{
    /* init code for LWIP */
    MX_LWIP_Init();
    /* USER CODE BEGIN InitialTask */
    SEGGER_RTT_Init();
    BSP_W25Qx_Init(&hw25q64, &hspi1);
    bsp_init_hub75();

    tcpServerTaskHandle = osThreadNew(tcpServerTask, NULL, &tcpServerTask_attributes);
    RefreshTaskHandle   = osThreadNew(RefreshTask, NULL, &RefreshTask_attributes);

    osThreadExit();
    /* Infinite loop */
    for (;;) {
        osDelay(1);
    }
    /* USER CODE END InitialTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void HalfSecTask(void *argument)
{
    uint8_t run_time = 0;
    for (;;) {
        HAL_IWDG_Refresh(&hiwdg);

        if (run_time >= 60)
            HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, 0);
        else
            run_time++;

        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);

        osDelay(500);
    }
}

void PointTestTask(void *argument)
{
    // HAL_TIM_Base_Stop_IT(&htim3);

    // pixel_map[32] = green;
    // convert_pixelmap();

    for (;;) {
        for (int i = 0; i < DISRAM_SIZE; i++) {
            pixel_map[i] = green;

            convert_pixelmap();
            osDelay(50);

            pixel_map[i] = black;
        }

        point_order_test(green, 1, 0);
        osDelay(500);
    }
}
/* USER CODE END Application */
