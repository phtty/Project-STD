/**
 * @file    app_light_sensor.c
 * @brief   环境光传感器 RTOS 任务 — 自注册 initcall，1s 周期自动调光
 */

#include "app_light_sensor.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"

static light_sensor_dev_t s_sensor_dev;

void app_light_sensor_task(void *argument)
{
    (void)argument;
    for (;;) {
        dev_light_sensor_auto_adjust(&s_sensor_dev);
        osDelay(1000);
    }
}

void app_light_sensor_init(void)
{
    dev_light_sensor_init(&s_sensor_dev, dev_display_get());

    const osThreadAttr_t attr = {
        .name       = "light_sensor_task",
        .stack_size = 128 * 4,
        .priority   = osPriorityLow,
    };
    osThreadNew(app_light_sensor_task, NULL, &attr);
}
sw_app_initcall(app_light_sensor_init);
