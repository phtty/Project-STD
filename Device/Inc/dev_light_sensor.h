/**
 * @file    dev_light_sensor.h
 * @brief   环境光传感器设备（ADC 采样 + 自动亮度调节）
 *
 * 自动调光输出范围由 min_level / max_level 限定，
 * 可通过 dev_light_sensor_set_range() 在运行时动态调整。
 */

#pragma once

#include <stdint.h>
#include "dev_display.h"

/** 自动调光默认最低亮度等级（出厂约束：不允许低于 4 级） */
#define LIGHT_SENSOR_MIN_LEVEL  4

/** 自动调光默认最高亮度等级（出厂约束：不允许高于 7 级） */
#define LIGHT_SENSOR_MAX_LEVEL  7

typedef struct {
    void *adc;
    dev_display_t *display;
    uint8_t min_level;  /* 自动调光最低亮度 (1~7) */
    uint8_t max_level;  /* 自动调光最高亮度 (1~7) */
} light_sensor_dev_t;

void dev_light_sensor_init(light_sensor_dev_t *dev, dev_display_t *display);
uint8_t dev_light_sensor_read(light_sensor_dev_t *dev);
void dev_light_sensor_auto_adjust(light_sensor_dev_t *dev);

/**
 * @brief  设置光敏自动调光的亮度范围
 * @param  dev      光传感器实例
 * @param  min      最低亮度等级 (1~7, 0=不关屏)
 * @param  max      最高亮度等级 (1~7)
 *
 * min/max 会被自动钳位到 1~7，且 min ≤ max。
 * 调用后下一次 auto_adjust 即以新范围输出。
 */
void dev_light_sensor_set_range(light_sensor_dev_t *dev, uint8_t min, uint8_t max);
