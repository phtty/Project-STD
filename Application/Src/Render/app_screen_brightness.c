/**
 * @file    app_screen_brightness.c
 * @brief   整屏门面 · 亮度与屏开关实现 —— 记期望值、置"待下发"、门控施加值
 *
 * 由 app_screen.c 拆出（簇 E）：亮度是**整屏**的属性 —— 级联下由主卡统一分发，
 * 从卡各自调光会在屏上留下亮度接缝，故 `app_screen_set_brightness()` 是唯一的
 * 亮度入口（光传感器也走它）。
 *
 * **开关与亮度分离**（CDSTD A0H / A2H）：
 *   · 开关状态 = `s_display_on`（默认 true，上电屏是亮的）；
 *   · 亮度期望值 = `s_bright_level` —— 它同时就是"关屏缓存"本身；
 *   · 实际施加到硬件的值 = `s_display_on ? s_bright_level : 0`。
 *
 * 关屏期间任何亮度来源（光敏 / 命令 / 级联）只会更新期望值，不会把屏点亮。
 * **"冻结"由命令层完成**：关屏前先 `app_light_sensor_set_fixed(当前亮度)` 停掉
 * 光敏跟随 —— 门面只负责门控，不依赖光敏模块（分层不反）。
 *
 * 作用到实屏走公开 API `dev_display_get()`，不 extern 门面的 `s_display_dev`；
 * 三个 static（开关 / 当前等级 / 待下发标志）全归自己。
 */

#include "app_screen.h"

#include "dev_display.h"

static volatile bool    s_bright_pending_flag;
static volatile uint8_t s_bright_level;
static volatile bool    s_display_on = true; /* 上电默认开屏（屏是亮的） */

void app_screen_set_brightness(uint8_t level)
{
    if (level > 7) level = 7;

    /* 关屏时硬件恒 0，但**期望值仍记 level** —— 这就是"缓存"，开屏时恢复它。
       光敏/命令/级联都从这里过，所以关屏期间谁也点不亮屏。 */
    uint8_t applied = s_display_on ? level : 0;

    dev_display_t *d = dev_display_get();
    if (d) dev_display_set_brightness(d, applied);

    s_bright_level        = level;
    s_bright_pending_flag = true; /* 由级联协议取走并广播给从卡（广播有效值） */
}

uint8_t app_screen_get_brightness(void)
{
    dev_display_t *d = dev_display_get();
    return d ? d->light_level : 0;
}

void app_screen_set_display_on(bool on)
{
    s_display_on = on;

    /* 立即重新施加：关 → 0；开 → 恢复到缓存的期望值。 */
    uint8_t applied = on ? s_bright_level : 0;

    dev_display_t *d = dev_display_get();
    if (d) dev_display_set_brightness(d, applied);

    s_bright_pending_flag = true; /* 让级联把开关状态同步给从卡 */
}

bool app_screen_is_display_on(void)
{
    return s_display_on;
}

bool app_screen_brightness_take_pending(uint8_t *level)
{
    if (!s_bright_pending_flag) return false;
    s_bright_pending_flag = false;

    /* **有效值**：关屏时为 0，级联据此让从卡一起灭；缓存的期望值不出门。 */
    if (level) *level = s_display_on ? s_bright_level : 0;
    return true;
}
