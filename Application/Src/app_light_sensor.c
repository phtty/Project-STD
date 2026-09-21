/**
 * @file    app_light_sensor.c
 * @brief   环境光传感器 RTOS 任务 — 自注册 initcall，1s 周期自动调光
 */

#include "app_light_sensor.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"
#include "app_screen.h"
#include "pl_task.h"

static light_sensor_dev_t s_sensor_dev;
osThreadId_t g_light_sensor_task_handle;

void app_light_sensor_task(void *argument)
{
    (void)argument;
    for (;;) {
        dev_light_sensor_auto_adjust(&s_sensor_dev);
        osDelay(1000);
    }
}

/** @brief 把算出的等级交给整屏门面 —— 主卡据此下发广播，从卡据此落本地 */
static void _apply_brightness(void *ctx, uint8_t level)
{
    (void)ctx;
    app_screen_set_brightness(level);
}

void app_light_sensor_init(void)
{
    dev_light_sensor_init(&s_sensor_dev, dev_display_get());
    s_sensor_dev.apply     = _apply_brightness;
    s_sensor_dev.apply_ctx = nullptr;

    /* **从卡不跑本任务**：亮度由主卡统一下发，本地再采一份就是两个来源打架，
     *  屏上会出现亮度接缝。判据用 app_screen_is_master() 而不是某个协议的状态 ——
     *  身份属于整屏，见 app_screen.h。 */
    if (!app_screen_is_master()) return;

    const osThreadAttr_t attr = {
        .name       = "light_sensor_task",
        .stack_size = 128 * 4,
        .priority   = osPriorityLow,
    };
    g_light_sensor_task_handle = pl_task_new(app_light_sensor_task, NULL, &attr);
}
sw_app_initcall(app_light_sensor_init);

void app_light_sensor_set_fixed(uint8_t level)
{
    s_sensor_dev.auto_adjust_enabled = false; /* 先停跟随, 防任务下个周期覆盖 */
    app_screen_set_brightness(level);         /* 与自动路径同一个入口，才能一并发给从卡 */
}

void app_light_sensor_resume(void)
{
    s_sensor_dev.auto_adjust_enabled = true;
    dev_light_sensor_auto_adjust(&s_sensor_dev); /* 立即生效, 消除 1s 周期延迟 */
}
