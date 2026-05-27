/**
 * @file    app_boot.c
 * @brief   系统启动编排器（过渡版）
 *
 * 接管 RTOS 生命周期，逐步替代 freertos.c 的 InitialTask。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "initcall.h"
#include "pl_iwdg.h"
#include "pl_rtc.h"
#include "pl_dwt.h"
#include "lwip.h"
#include "SEGGER_RTT.h"
#include "dev_flash_font.h"
#include "protocol.h"
#include "udp_app.h"

static font_flash_dev_t g_font_flash;

/* ---- 外部：freertos.c 的 HalfSecTask ---- */
extern void MX_FREERTOS_Init(void);

static void init_task(void *argument);

void app_boot(void)
{
    const osThreadAttr_t attr = {
        .name       = "init_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityHigh,
    };

    osKernelInitialize();
    osThreadNew(init_task, NULL, &attr);
    osKernelStart();
}

static void init_task(void *argument)
{
    (void)argument;

    /* 网络栈 */
    MX_LWIP_Init();

    /* 调试输出 */
    SEGGER_RTT_Init();

    /* W25Qxx 字库 Flash */
    dev_font_flash_init(&g_font_flash);

    /* 协议分发（过渡期仍用旧 protocol.c） */
    channel_init();

    /* 网络事件标志 */
    extern osEventFlagsId_t netEventFlagsHandle;
    netEventFlagsHandle = osEventFlagsNew(NULL);

    /* 网络任务 */
    extern osThreadId_t udpManageTaskHandle;
    extern const osThreadAttr_t udpManageTask_attributes;
    udpManageTaskHandle = osThreadNew(udpManageTask, NULL, &udpManageTask_attributes);

    /* 半秒任务（喂狗 + RTC 备份），从 freertos.c 继承 */
    MX_FREERTOS_Init();

    printf("\nInit Task Done\n");

    osThreadExit();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    for (;;);
}

void vConfigureTimerForRunTimeStats(void)
{
    pl_dwt_init();
}

uint32_t ulGetRunTimeCounterValue(void)
{
    return pl_dwt_get_cycles();
}
