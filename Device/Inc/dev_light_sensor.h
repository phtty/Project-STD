/**
 * @file    dev_light_sensor.h
 * @brief   环境光传感器设备（ADC 采样 + 自动亮度调节）
 */

#pragma once

#include <stdint.h>
#include "dev_display.h"

typedef struct {
    void *adc;
    dev_display_t *display;
} light_sensor_dev_t;

void dev_light_sensor_init(light_sensor_dev_t *dev, dev_display_t *display);
uint8_t dev_light_sensor_read(light_sensor_dev_t *dev);
void dev_light_sensor_auto_adjust(light_sensor_dev_t *dev);
