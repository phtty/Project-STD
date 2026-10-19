/**
 * @file    app_screen_brightness.c
 * @brief   整屏门面 · 亮度与屏开关（OE 输出使能）实现 —— 记期望值、置"待下发"、门控施加值
 *
 * 由 app_screen.c 拆出（簇 E）：亮度是**整屏**的属性 —— 级联下由主卡统一分发，
 * 从卡各自调光会在屏上留下亮度接缝，故 `app_screen_set_brightness()` 是唯一的
 * 亮度入口（光传感器也走它）。
 *
 * **开关与亮度分离**（CDSTD A0H / A2H）：
 *   · 开关状态 = `s_oe_enabled`（面板输出使能，默认 true —— 上电屏是亮的）；
 *   · 亮度期望值 = `s_bright_level` —— 它同时就是"关屏缓存"本身；
 *   · 实际施加到硬件的值 = `s_oe_enabled ? s_bright_level : 0`。
 *
 * 关屏期间任何亮度来源（光敏 / 命令 / 级联）只会更新期望值，不会把屏点亮。
 * **不冻结亮度**：开关只切输出，光敏跟随与期望值都不动 —— 关屏期间自动跟随照常
 * 更新期望值（硬件仍 0），开屏就用"当时的亮度与当时的模式"。
 *
 * 作用到实屏走公开 API `dev_display_get()`，不 extern 门面的 `s_display_dev`；
 * 三个 static（输出使能 / 期望亮度 / 待下发标志）全归自己。
 */

#include "app_screen.h"

#include "dev_display.h"

static volatile bool    s_bright_pending_flag;
static volatile uint8_t s_bright_level;
static volatile bool    s_oe_enabled = true; /* 上电默认开屏（面板输出使能） */

/** @brief 把"输出使能 + 期望亮度"施加到硬件：关屏恒 0，开屏用期望值
 *
 *  单独抽出来而不是各写一遍：`set_brightness()` 与 `oe_enable/oe_disable()` 的"立即
 *  重新施加"必须是同一段逻辑，否则三处容易漂。 */
static void _oe_apply(void)
{
    dev_display_t *d = dev_display_get();
    if (d) dev_display_set_brightness(d, s_oe_enabled ? s_bright_level : 0);
}

void app_screen_set_brightness(uint8_t level)
{
    if (level > 7) level = 7;

    /* 期望值（= "关屏缓存"）照记；关屏时 _oe_apply() 只会把它压成 0。 */
    s_bright_level = level;
    _oe_apply();

    s_bright_pending_flag = true; /* 由级联协议取走并广播给从卡（广播有效值） */
}

uint8_t app_screen_get_brightness(void)
{
    dev_display_t *d = dev_display_get();
    return d ? d->light_level : 0;
}

void app_screen_oe_enable(void)
{
    s_oe_enabled = true;
    _oe_apply();                  /* 硬件恢复到缓存的期望值 */
    s_bright_pending_flag = true; /* 让级联把"开屏"同步给从卡 */
}

void app_screen_oe_disable(void)
{
    s_oe_enabled = false;
    _oe_apply();                  /* 硬件施加 0，期望值保留 */
    s_bright_pending_flag = true; /* 让级联把"关屏"同步给从卡 */
}

bool app_screen_is_oe_enabled(void)
{
    return s_oe_enabled;
}

bool app_screen_brightness_take_pending(uint8_t *level)
{
    if (!s_bright_pending_flag) return false;
    s_bright_pending_flag = false;

    /* **有效值**：关屏时为 0，级联据此让从卡一起灭；缓存的期望值不出门。 */
    if (level) *level = s_oe_enabled ? s_bright_level : 0;
    return true;
}
