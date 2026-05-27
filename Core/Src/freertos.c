/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
#include "pl_iwdg.h"
#include "pl_rtc.h"

/* Private typedef -----------------------------------------------------------*/
typedef StaticTask_t osStaticThreadDef_t;

/* Private define ------------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/
osSemaphoreId_t test_semaphore;
osEventFlagsId_t SW123_Event;

/* Private variables ---------------------------------------------------------*/
osThreadId_t HalfSecTaskHandle;
const osThreadAttr_t HalfSecTask_attributes = {
    .name       = "HalfSecTask",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

/* InitialTask / InitTask 已迁移至 Application/Src/app_boot.c */

/* Private function prototypes -----------------------------------------------*/
void HalfSecTask(void *argument);

void MX_FREERTOS_Init(void);

/**
 * @brief  FreeRTOS initialization
 * @param  None
 * @retval None
 */
void MX_FREERTOS_Init(void)
{

    test_semaphore = osSemaphoreNew(1, 0, NULL);
    configASSERT(test_semaphore != NULL);

    /* InitTask 已迁移至 Application/Src/app_boot.c */
    HalfSecTaskHandle = osThreadNew(HalfSecTask, NULL, &HalfSecTask_attributes);

}

/* Private application code --------------------------------------------------*/
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
