/**
 * @file    app_boot.c
 * @brief   系统启动编排（Application 层组合根）
 *
 * app_boot() 初始化 FreeRTOS 并创建 init_task 作为第一个用户任务。
 * init_task 作为 "进程树根节点"，依次派生 half_sec_task、网络通道任务。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "initcall.h"
#include "pl_iwdg.h"
#include "pl_rtc.h"
#include "app_dispatch.h"
#include "dev_eth.h"
#include "app_tcp_server.h"
#include "app_tcp_client.h"
#include "app_mqtt.h"
#include "app_udp.h"
#include "pl_uart.h"
#include "app_rs232.h"
#include "app_rs485.h"
#include "app_ldi.h"
#include "app_onbon.h"
#include "pl_dwt.h"

static void init_task(void *argument);
static void half_sec_task(void *argument);

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

/* ---- init_task: 进程树根节点，派生所有子任务 ---- */
static void init_task(void *argument)
{
    dev_eth_start();
    sw_board_init();

    /* 派生 half_sec_task（喂狗 + RTC） */
    const osThreadAttr_t hst_attr = {
        .name       = "half_sec_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityLow,
    };
    osThreadNew(half_sec_task, NULL, &hst_attr);

    /* 派生串口通道任务 */
    app_rs232_start();
    app_rs485_start();

    /* 派生网络通道任务 */
    app_tcp_server_start();
    app_tcp_client_start();
    app_udp_start();
    // app_mqtt_start();

    osDelay(60000);
    onbon_ctx_t ctx = {0};
    onbon_ctx_init(&ctx);
    ctx.color = ONBON_COLOR_RED;
    onbon_send_text(&ctx, "车道关闭×");

    osThreadExit(); /* 全部子任务已创建，销毁自身 */
}

/* ---- half_sec_task: 500ms 喂狗 / 60s 写 RTC 运行标志 ---- */
static void half_sec_task(void *argument)
{
    uint32_t run_time = 0;

    for (;;) {
        pl_iwdg_refresh(pl_iwdg_get_handle());

        if (run_time >= 60)
            pl_rtc_bkup_write(pl_rtc_get_handle(), 1, 0);
        else
            run_time++;

        osDelay(500);
    }
}

void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
    for (;;);
}

/* ---- 运行统计（DWT 周期计数器）---- */
void vConfigureTimerForRunTimeStats(void)
{
    pl_dwt_init();
}

uint32_t ulGetRunTimeCounterValue(void)
{
    return pl_dwt_get_cycles();
}
