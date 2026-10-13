/**
 * @file    app_screen_brightness.c
 * @brief   整屏门面 · 亮度实现 —— 设本地亮度并置"待下发"标志
 *
 * 由 app_screen.c 拆出（簇 E）：亮度是**整屏**的属性 —— 级联下由主卡统一分发，
 * 从卡各自调光会在屏上留下亮度接缝，故 `app_screen_set_brightness()` 是唯一的
 * 亮度入口（光传感器也走它）。
 *
 * 作用到实屏走公开 API `dev_display_get()`，不 extern 门面的 `s_display_dev`；
 * 两个 static（当前等级 / 待下发标志）全归自己。
 */

#include "app_screen.h"

#include "dev_display.h"

static volatile bool    s_bright_pending_flag;
static volatile uint8_t s_bright_level;

void app_screen_set_brightness(uint8_t level)
{
    if (level > 7) level = 7;
    dev_display_t *d = dev_display_get();
    if (d) dev_display_set_brightness(d, level);

    s_bright_level        = level;
    s_bright_pending_flag = true; /* 由级联协议取走并广播给从卡 */
}

uint8_t app_screen_get_brightness(void)
{
    dev_display_t *d = dev_display_get();
    return d ? d->light_level : 0;
}

bool app_screen_brightness_take_pending(uint8_t *level)
{
    if (!s_bright_pending_flag) return false;
    s_bright_pending_flag = false;
    if (level) *level = s_bright_level;
    return true;
}
