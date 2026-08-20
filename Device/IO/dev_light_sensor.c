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
    dev->adc     = pl_adc_get_handle();
    dev->display = display;
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
       映射: ADC 0(最亮) → level 7,  ADC 3000+(最暗) → level 1
       display 支持 0~7 (0=关, 7=最亮)，这里只出 1~7（不关屏） */
    uint8_t level = 8 - (uint8_t)(temp_val / 500);
    if (level < 1) level = 1;
    if (level > 7) level = 7;
    return level;
}

void dev_light_sensor_auto_adjust(light_sensor_dev_t *dev)
{
    static uint8_t last_light = 0; /* 上一次采样的亮度等级 (读取值域 1~7，0 表示尚未采样) */
    static uint8_t stable_cnt = 0; /* 连续采样到相同等级的计数 */

    uint8_t new_light = dev_light_sensor_read(dev);

    if (new_light == last_light) {
        /* 连续 3 次采样到同一等级才修改亮度，过滤 ADC 抖动，避免亮度来回跳 */
        if (stable_cnt < 3 && ++stable_cnt == 3)
            dev->display->light_level = new_light;
    } else {
        last_light = new_light;
        stable_cnt = 1;
    }
}
