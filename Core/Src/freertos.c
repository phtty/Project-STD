/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
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
#include "pl_iwdg.h"
#include "pl_rtc.h"

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
/* USER CODE END Variables */

/* InitialTask / InitTask 已迁移至 Application/Src/app_boot.c */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void HalfSecTask(void *argument);
/* USER CODE END FunctionPrototypes */

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
    /* USER CODE END RTOS_MUTEX */

    /* USER CODE BEGIN RTOS_SEMAPHORES */
    test_semaphore = osSemaphoreNew(1, 0, NULL);
    configASSERT(test_semaphore != NULL);
    /* USER CODE END RTOS_SEMAPHORES */

    /* USER CODE BEGIN RTOS_TIMERS */
    /* USER CODE END RTOS_TIMERS */

    /* USER CODE BEGIN RTOS_QUEUES */
    /* USER CODE END RTOS_QUEUES */

    /* InitTask 已迁移至 Application/Src/app_boot.c */
    HalfSecTaskHandle = osThreadNew(HalfSecTask, NULL, &HalfSecTask_attributes);

    /* USER CODE BEGIN RTOS_THREADS */
    /* USER CODE END RTOS_THREADS */

    /* USER CODE BEGIN RTOS_EVENTS */
    /* USER CODE END RTOS_EVENTS */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void HalfSecTask(void *argument)
{
    uint32_t run_time = 0;

    for (;;) {
        pl_iwdg_refresh(pl_iwdg_get_handle());

        if (run_time >= 60)
            pl_rtc_bkup_write(pl_rtc_get_handle(), RTC_BKP_DR1, 0);
        else
            run_time++;

        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);

        osDelay(500);
    }
}

/* vApplicationStackOverflowHook / vConfigureTimerForRunTimeStats / ulGetRunTimeCounterValue
   已迁移至 Application/Src/app_boot.c */
/* USER CODE END Application */
