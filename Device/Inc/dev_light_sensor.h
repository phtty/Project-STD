/**
 * @file    dev_light_sensor.h
 * @brief   环境光传感器设备（ADC 采样 + 自动亮度调节）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dev_display.h"

typedef struct {
    void *adc;
    dev_display_t *display;
    volatile bool auto_adjust_enabled; /* false = 停止跟随, 亮度由外部指定 */

    /* 亮度应用回调。**非 NULL 时由它套用等级**，NULL 则直接写 display->light_level。
     *
     *  为什么要留这条缝：本模块属 Device 层，不能反向依赖 Application。而亮度在
     *  级联场景下要经 app_screen 统一下发给所有卡 —— 由 Device 层直接写
     *  display->light_level 的话，从卡永远收不到主卡的调光。 */
    void (*apply)(void *ctx, uint8_t level);
    void *apply_ctx;
} light_sensor_dev_t;

void dev_light_sensor_init(light_sensor_dev_t *dev, dev_display_t *display);
uint8_t dev_light_sensor_read(light_sensor_dev_t *dev);
void dev_light_sensor_auto_adjust(light_sensor_dev_t *dev);
