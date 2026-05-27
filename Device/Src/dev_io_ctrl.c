/**
 * @file    dev_io_ctrl.c
 * @brief   IO 控制设备实现（车道灯 PD14, 闪光灯 PD15）
 */

#include "dev_io_ctrl.h"

#include "pl_gpio.h"

void dev_io_lane_light(bool enable)
{
    pl_gpio_write(PL_PORT_D, 14, enable);
}

void dev_io_flash_light(bool enable)
{
    pl_gpio_write(PL_PORT_D, 15, enable);
}
