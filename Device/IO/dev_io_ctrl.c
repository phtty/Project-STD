/**
 * @file    dev_io_ctrl.c
 * @brief   IO 控制设备实现（车道灯 PD14, 闪光灯 PD15）
 */

#include "dev_io_ctrl.h"

#include <stdbool.h>
#include "initcall.h"
#include "pl_gpio.h"

void dev_io_ctrl_init(void)
{
    /* GPIO 已由 MX_GPIO_Init（hw_pl_initcall）配置，此处确保初始关闭 */
    pl_gpio_write(PL_PORT_D, 14, false);
    pl_gpio_write(PL_PORT_D, 15, false);
}
hw_dev_initcall(dev_io_ctrl_init);

void dev_io_lane_light(bool enable)
{
    pl_gpio_write(PL_PORT_D, 14, enable);
}

void dev_io_flash_light(bool enable)
{
    pl_gpio_write(PL_PORT_D, 15, enable);
}
