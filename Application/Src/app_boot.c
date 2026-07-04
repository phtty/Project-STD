/**
 * @file    app_boot.c
 * @brief   系统启动编排器
 *
 * 职责：RTOS 生命周期 + 硬件无关的模块初始化 + HalfSecTask
 */

#include <stdint.h>
#include <stdio.h>

#include "cmsis_os.h"
#include "initcall.h"
#include "pl_iwdg.h"
#include "pl_rtc.h"
#include "pl_gpio.h"
#include "pl_dwt.h"
#include "dev_eth.h"
#include "app_tcp_server.h"
#include "app_tcp_client.h"
#include "app_test.h"
#include "app_key.h"

static void init_task(void *argument);

/* ---- HalfSecTask: 500ms 喂狗 / 60s RTC 备份 / LED 翻转 ---- */
static void half_sec_task(void *argument)
{
    (void)argument;
    uint32_t run_time = 0;
    bool led_state    = false;

    for (;;) {
        pl_iwdg_refresh(pl_iwdg_get_handle());

        if (run_time >= 60)
            pl_rtc_bkup_write(pl_rtc_get_handle(), 1, 0);
        else
            run_time++;

        led_state = !led_state;
        pl_gpio_write(PL_PORT_D, 9, led_state); /* LED = PD9 */

        osDelay(500);
    }
}

/* ---- 启动入口 ---- */
void app_boot(void)
{
    const osThreadAttr_t attr = {
        .name       = "init_task",
        .stack_size = 512 * 4,
        .priority   = osPriorityHigh,
    };

    osKernelInitialize();
    osThreadNew(init_task, NULL, &attr);
    osKernelStart();
}

static void init_task(void *argument)
{
    (void)argument;

    dev_eth_start();
    sw_board_init(); /* sw_initcall 自注册：协议 + 通道任务 */

    /* 半秒周期任务 */
    const osThreadAttr_t hst_attr = {
        .name       = "half_sec_task",
        .stack_size = 128 * 4,
        .priority   = osPriorityLow,
    };
    osThreadNew(half_sec_task, NULL, &hst_attr);

    app_tcp_server_start();
    app_tcp_client_start();

    app_test_run();

    printf("\nInit Task Done\n");

    osThreadExit();
}

/* ---- FreeRTOS 钩子 ---- */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
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
