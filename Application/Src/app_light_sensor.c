/**
 * @file    app_light_sensor.c
 * @brief   环境光传感器 RTOS 任务实现
 */

#include "app_light_sensor.h"
#include "cmsis_os2.h"

static light_sensor_dev_t *g_sensor;

void app_light_sensor_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (g_sensor)
            dev_light_sensor_auto_adjust(g_sensor);
        osDelay(1000);
    }
}
