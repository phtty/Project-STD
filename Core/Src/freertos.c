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
#include "lwip.h"
#include "protocol.h"
#include "SEGGER_RTT.h"
#include "w25qxx.h"
#include "render.h"
#include "light.h"
#include "tcp_server_app.h"
#include "tcp_client_app.h"
#include "mqtt_app.h"
#include "udp_app.h"
#include "RS232.h"
#include "RS485.h"

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
osSemaphoreId_t test_semaphore;
osEventFlagsId_t SW123_Event;
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osThreadId_t HalfSecTaskHandle;
const osThreadAttr_t HalfSecTask_attributes = {
    .name       = "HalfSecTask",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

osThreadId_t PointTestTaskHandle;
const osThreadAttr_t PointTestTask_attributes = {
    .name       = "PointTestTask",
    .stack_size = 1024 * 4,
    .priority   = (osPriority_t)osPriorityAboveNormal,
};

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
void MX_FREERTOS_Init(void);

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
    test_semaphore = osSemaphoreNew(1, 0, NULL);
    configASSERT(test_semaphore != NULL);
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
    BSP_W25Qx_Init(&hw25q256, &hspi1);
    channel_init();

    // 创建事件标志等待网络就绪
    netEventFlagsHandle = osEventFlagsNew(NULL);

    // mqttManageTaskHandle = osThreadNew(mqttManageTask, NULL, &mqttManageTask_attributes);
    udpManageTaskHandle = osThreadNew(udpManageTask, NULL, &udpManageTask_attributes);
    // tcpServerTaskHandle = osThreadNew(tcpServerTask, NULL, &tcpServerTask_attributes);
    // tcpClientTaskHandle = osThreadNew(tcpClientTask, NULL, &tcpClientTask_attributes);
    // rs2321ManageTaskHandle = osThreadNew(rs2321ManageTask, NULL, &rs2321ManageTask_attributes);
    // rs2322ManageTaskHandle = osThreadNew(rs2322ManageTask, NULL, &rs2322ManageTask_attributes);
    rs485ManageTaskHandle = osThreadNew(rs485ManageTask, NULL, &rs485ManageTask_attributes);

    // autoAdjLightTaskHandle = osThreadNew(autoAdjLightTask, NULL, &autoAdjLightTask_attributes);

    // RefreshTaskHandle = osThreadNew(RefreshTask, NULL, &RefreshTask_attributes);
    // PointTestTaskHandle = osThreadNew(PointTestTask, NULL, &PointTestTask_attributes);
    // RenderString(0, 0, "测试", strlen("测试"), green, font_16, font_ht);

    printf("\nInit Task Done\n");

    // osThreadExit();
    osThreadSuspend(InitTaskHandle);
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
    uint32_t run_time = 0;

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
    light_level = 7;
    HAL_TIM_Base_Start_IT(&htim3);
    HAL_TIM_Base_Start_IT(&htim4);

    for (;;) {
        for (int i = SCAN_LINE_PIXEL_NUM - 50; i < DISRAM_SIZE; i++) {
            pixel_map[i] = green;

            convert_pixelmap();
            osDelay(50);

            pixel_map[i] = black;
        }

        // point_order_test(green, 1, 0);
        // osDelay(500);
    }
}

// 任务栈溢出钩子函数，用来检测哪个任务有栈溢出
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    while (1);
}

/* 初始化 DWT */
void vConfigureTimerForRunTimeStats(void)
{
    /* 使能 DWT 模块 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    /* 清零计数器 */
    DWT->CYCCNT = 0;
    /* 使能计数器 */
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* 读取当前计数值（CPU 周期数） */
uint32_t ulGetRunTimeCounterValue(void)
{
    return DWT->CYCCNT;
}
/* USER CODE END Application */
