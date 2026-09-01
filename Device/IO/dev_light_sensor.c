/**
 * @file    dev_light_sensor.c
 * @brief   环境光传感器设备实现
 */

#include "dev_light_sensor.h"

#include "initcall.h"
#include "pl_adc.h"

#define MEAN_PARAMETER 8

void dev_light_sensor_init(light_sensor_dev_t *dev, dev_display_t *display)
{
    dev->adc       = pl_adc_get_handle();
    dev->display   = display;
    dev->min_level = 1;
    dev->max_level = DEV_DISPLAY_BRIGHTNESS_MAX;
}

void dev_light_sensor_set_range(light_sensor_dev_t *dev, uint8_t min, uint8_t max)
{
    if (min < 1) min = 1;
    if (min > DEV_DISPLAY_BRIGHTNESS_MAX) min = DEV_DISPLAY_BRIGHTNESS_MAX;
    if (max < 1) max = 1;
    if (max > DEV_DISPLAY_BRIGHTNESS_MAX) max = DEV_DISPLAY_BRIGHTNESS_MAX;
    if (min > max) min = max;

    dev->min_level = min;
    dev->max_level = max;
}

uint8_t dev_light_sensor_read(light_sensor_dev_t *dev)
{
    uint32_t temp_val = 0;

    for (uint32_t i = 0; i < MEAN_PARAMETER; i++) {
        uint32_t val;
        if (pl_adc_read(dev->adc, &val, 100) == 0)
            temp_val += val;
    }

    temp_val /= MEAN_PARAMETER;
    if (temp_val > 4000) temp_val = 4000;

    /* ADC 值越大 = 光越暗 (LDR 分压: 光强↓ → 电阻↑ → 电压↑)
       映射: ADC 0(最亮) → level 8,  ADC 4000(最暗) → level 0 */
    uint8_t level = 8 - (uint8_t)(temp_val / 500);
    if (level < 1) level = 1;
    if (level > DEV_DISPLAY_BRIGHTNESS_MAX) level = DEV_DISPLAY_BRIGHTNESS_MAX;

    /* 按配置范围钳位 */
    if (level < dev->min_level) level = dev->min_level;
    if (level > dev->max_level) level = dev->max_level;

    return level;
}

void dev_light_sensor_auto_adjust(light_sensor_dev_t *dev)
{
    static uint8_t old_light = 0;
    uint8_t new_light        = dev_light_sensor_read(dev);

    if (new_light != old_light)
        dev->display->light_level = new_light;

    old_light = new_light;
}
