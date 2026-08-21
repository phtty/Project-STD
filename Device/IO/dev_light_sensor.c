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
    dev->adc                 = pl_adc_get_handle();
    dev->display             = display;
    dev->auto_adjust_enabled = true; /* 静态实例零初始化，必须显式置 true */
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
    if (!dev->auto_adjust_enabled)
        return;

    static uint8_t old_light = 0;
    uint8_t new_light        = dev_light_sensor_read(dev);

    /* 环境变化，或 light_level 被外部改写（如 RLS 手动亮度/关屏后恢复自动）时强制写回 */
    if (new_light != old_light || dev->display->light_level != new_light)
        dev->display->light_level = new_light;

    old_light = new_light;
}
