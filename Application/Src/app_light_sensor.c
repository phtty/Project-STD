/**
 * @file    app_light_sensor.c
 * @brief   环境光传感器 RTOS 任务 — 自注册 initcall，1s 周期自动调光
 */

#include "app_light_sensor.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"
#include "pl_uart.h"

#include "app_key.h"
#include "app_msl.h"
#include "bcc_utils.h"

static light_sensor_dev_t s_sensor_dev;
osThreadId_t g_light_sensor_task_handle;

static void send_light_adjust(uint8_t light_lev)
{
    // 与显示内容发送共用 UART6 和 msl_tx_lock，避免帧竞争
    osMutexAcquire(msl_tx_lock, osWaitForever);

    uint8_t _buf[sizeof(msl_frame_t) + 2] = {0};
    msl_frame_t *f                        = (msl_frame_t *)_buf;

    f->head[0] = 0x5a;
    f->head[1] = 0x5a;

    f->addr = 0;

    f->length[0] = 0;
    f->length[1] = 1;

    f->cmd = MSL_CMD_LIGHTLEVEL;

    f->data_bcc[0] = light_lev;

    // bcc校验
    f->data_bcc[1] = bcc_calcu(&(f->addr), sizeof(msl_frame_t) - 1);

    // 发送数据帧
    pl_uart_send(pl_uart_get_handle(PL_UART6), _buf, sizeof(msl_frame_t) + 2, 50);

    osMutexRelease(msl_tx_lock);
}

void app_light_sensor_task(void *argument)
{
    (void)argument;
    osDelay(10000); // 延迟开启调光，避免和发送显示内容撞上
    for (;;) {
        dev_light_sensor_auto_adjust(&s_sensor_dev);
        send_light_adjust(s_sensor_dev.display->light_level);
        osDelay(10000);
    }
}

void app_light_sensor_init(void)
{
    // 若不是主卡则不开自动亮度
    if (0 != ((dev_key_get_state(DEV_KEY_DIP1) & 0b01) | ((dev_key_get_state(DEV_KEY_DIP2) << 1) & 0b10)))
        return;

    dev_light_sensor_init(&s_sensor_dev, dev_display_get());

    const osThreadAttr_t attr = {
        .name       = "light_sensor_task",
        .stack_size = 128 * 4,
        .priority   = osPriorityLow,
    };
    g_light_sensor_task_handle = osThreadNew(app_light_sensor_task, NULL, &attr);
}
sw_app_initcall(app_light_sensor_init);

light_sensor_dev_t *app_light_sensor_get(void)
{
    return &s_sensor_dev;
}
