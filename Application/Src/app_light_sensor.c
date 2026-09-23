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

static dev_light_sensor_dev_t s_sensor_dev;
osThreadId_t g_light_sensor_task_handle;

void app_light_sensor_task(void *argument)
{
    (void)argument;
    for (;;) {
        /* **主从判断放在循环体里，不放建任务时**：身份现在可以**运行期**变
           （按键认领主卡 / 收到识别帧降级），任务按上电身份建或不建之后就再也
           改不回来。从卡不采光 —— 亮度由主卡统一下发，本地再采一份就是两个来源
           打架，屏上会出现亮度接缝（判据用 app_screen_is_master()，身份属于整屏，
           见 app_screen.h）。 */
        if (app_screen_is_master()) dev_light_sensor_auto_adjust(&s_sensor_dev);
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

    /* **无条件建任务**（主从判据已挪进任务循环体，见上）。
      顺带修一个既有隐患：`g_light_sensor_task_handle` 在从卡上以前是 NULL，
      而 app_factory_test.c 会对它 osThreadSuspend/Resume —— 传 NULL 会挂起调用者自己。 */
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
