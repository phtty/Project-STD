/**
 * @file    test_screen_brightness.c
 * @brief   整屏门面 · 亮度与屏开关：开关与亮度分离、关屏缓存、级联有效值
 *
 * **为什么需要这个测试**：CDSTD 的 A0H（屏开关）与 A2H（亮度）是两个独立状态，
 * 但硬件只有"亮度"一个落点（`dev_display_t.light_level`，0 = OE 恒消隐）。实现上
 * 两者很容易被写成一件事 —— 典型缺陷是"关屏 = 设亮度 0"，于是关屏前的档位丢了，
 * 或者关屏后光敏/命令一改亮度屏又亮起来。这些缺陷在屏上只表现为"关不掉"或
 * "开屏后亮度不对"，靠人眼很难归因，所以这里用记录桩把**每次施加到硬件的值**钉住。
 *
 * 被测代码是生产源码本体：直接 include `app_screen_brightness.c`（三个状态都是
 * file-static，从外部够不着；这样也顺带覆盖了测试专属的复位路径）。
 *
 * 记录桩只做两件事：`dev_display_get()` 回一个静态实例；`dev_display_set_brightness()`
 * 记下传入等级（同时像真实现一样写 `light_level`）。不引入新的公共桩文件。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dev_display.h"
#include "app_screen.h"

/* ================================================================
 *  记录桩：硬件亮度的唯一落点
 * ================================================================ */

static dev_display_t s_dev;

static int     s_set_calls; /* 施加次数（看"重复关屏是否重新施加"） */
static uint8_t s_set_level; /* 最后一次施加的等级；0xFF = 还没施加过 */
static bool    s_set_seen;  /* 是否施加过（区分"施加了 0"与"没施加"） */

void dev_display_set_brightness(dev_display_t *dev, uint8_t level)
{
    s_set_calls++;
    s_set_level = level;
    s_set_seen  = true;
    if (dev) dev->light_level = level; /* 与真实现一致：硬件值就是 light_level */
}

dev_display_t *dev_display_get(void) { return &s_dev; }

/* ---- 被测：生产源码本体（开关 / 期望值 / 待下发标志都是 file-static）---- */
#include "../Application/Src/Render/app_screen_brightness.c"

/* ================================================================
 *  夹具
 * ================================================================ */

/** @brief 只复位记录桩，**不动门面状态** —— 用于观察"下一次施加了什么" */
static void stub_reset(void)
{
    s_set_calls = 0;
    s_set_seen  = false;
    s_set_level = 0xFF;
    memset(&s_dev, 0, sizeof(s_dev));
    s_dev.light_level = 7; /* 模组上电默认亮度，与真实现一致 */
}

/** @brief 复位到上电等价态：开屏、期望值 0、无待下发
 *
 *  **不能用于 case_power_on_oe_enabled**：那条用例检查的是真实的 BSS 初值。 */
static void brightness_reset(void)
{
    stub_reset();
    s_oe_enabled          = true;
    s_bright_level        = 0;
    s_bright_pending_flag = false;
}

/* ---- 断言 ---- */
static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  用例
 * ================================================================ */

/** @brief 上电默认是**开屏** —— 默认值写反的话，上电后所有亮度设置都会被门控成 0
 *
 *  **必须是 main 的第一个用例，且此前不得调用任何复位夹具**：reset 会把开关强行
 *  写成 true，那样就测不出生产代码的初值了。 */
static void case_power_on_oe_enabled(void)
{
    TEST_BEGIN("上电默认开屏（oe_enabled == true）");

    CHECK_MSG(app_screen_is_oe_enabled(),
              "上电默认应为开屏（s_oe_enabled 初值 true），实际为关屏");
}

/** @brief 设亮度：施加到硬件、开关状态不变、待下发给出新等级 */
static void case_set_brightness_applies_and_pends(void)
{
    TEST_BEGIN("设亮度 5 → 硬件 5、开关仍为开、待下发 5");

    brightness_reset();
    app_screen_set_brightness(5);

    CHECK_MSG(s_set_seen && s_set_level == 5,
              "亮度 5 应施加到硬件（调用 %d 次，最后施加 %u）",
              s_set_calls, (unsigned)s_set_level);
    CHECK_MSG(app_screen_is_oe_enabled(), "设亮度不应改变开关状态");
    CHECK_MSG(app_screen_get_brightness() == 5,
              "硬件亮度应为 5，实际 %u", (unsigned)app_screen_get_brightness());

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl), "设亮度后应有待下发标志");
    CHECK_MSG(lvl == 5, "待下发亮度应为 5，实际 %u", (unsigned)lvl);
}

/** @brief >7 钳到 7（与既有亮度入口的契约一致，协议 0x38 与 0x37 视觉相同的前提） */
static void case_brightness_clamped(void)
{
    TEST_BEGIN("亮度 >7 钳到 7（9 → 7）");

    brightness_reset();
    app_screen_set_brightness(9);

    CHECK_MSG(s_set_level == 7, "9 应钳到 7 再施加，实际 %u", (unsigned)s_set_level);
    CHECK_MSG(app_screen_get_brightness() == 7,
              "硬件亮度应为 7，实际 %u", (unsigned)app_screen_get_brightness());

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl), "钳位后应置待下发");
    CHECK_MSG(lvl == 7, "待下发也应是钳位后的 7，实际 %u", (unsigned)lvl);
}

/** @brief 关屏门控：硬件立即施加 0，且**待下发是有效值 0**（级联据此让从卡一起灭） */
static void case_oe_disable_gates_hardware(void)
{
    TEST_BEGIN("关屏 → 硬件 0、待下发有效值 0");

    brightness_reset();
    app_screen_set_brightness(5); /* 先有个非零期望值 */
    stub_reset();                 /* 只清记录，门面状态（含期望值 5）保留 */

    app_screen_oe_disable();

    CHECK_MSG(!app_screen_is_oe_enabled(), "关屏后 is_oe_enabled 应为 false");
    CHECK_MSG(s_set_calls == 1 && s_set_level == 0,
              "关屏应立即把硬件施加为 0（调用 %d 次，最后施加 %u）",
              s_set_calls, (unsigned)s_set_level);
    CHECK_MSG(app_screen_get_brightness() == 0,
              "关屏后硬件亮度应为 0，实际 %u", (unsigned)app_screen_get_brightness());

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl), "关屏应置待下发（否则从卡不灭）");
    CHECK_MSG(lvl == 0, "关屏时待下发必须是有效值 0，实际 %u", (unsigned)lvl);
}

/** @brief 关屏期间改亮度：只更新期望值（缓存），硬件仍为 0、待下发仍为 0 */
static void case_cache_retained_while_off(void)
{
    TEST_BEGIN("关屏期间设亮度 3 → 硬件仍 0、期望值变 3、待下发仍 0");

    brightness_reset();
    app_screen_set_brightness(5);
    app_screen_oe_disable();
    stub_reset();

    app_screen_set_brightness(3);

    CHECK_MSG(s_set_calls == 1 && s_set_level == 0,
              "关屏期间设亮度，硬件仍必须是 0（调用 %d 次，最后施加 %u）",
              s_set_calls, (unsigned)s_set_level);
    CHECK_MSG(s_bright_level == 3,
              "期望值应更新为 3（这就是关屏缓存），实际 %u", (unsigned)s_bright_level);
    CHECK_MSG(app_screen_get_brightness() == 0,
              "关屏期间硬件亮度必须保持 0，实际 %u", (unsigned)app_screen_get_brightness());

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl), "关屏期间改亮度仍应置待下发");
    CHECK_MSG(lvl == 0, "关屏期间待下发是有效值 0（不是缓存的 3），实际 %u", (unsigned)lvl);
}

/** @brief 开屏恢复：恢复的是**关屏期间最后设置的期望值**（3），不是关屏前的值（5） */
static void case_restore_on_oe_enable(void)
{
    TEST_BEGIN("开屏恢复关屏期间最后设置的期望值 3（不是关屏前的 5）");

    brightness_reset();
    app_screen_set_brightness(5); /* 关屏前的值 —— 若实现没缓存，开屏会错误地用它 */
    app_screen_oe_disable();
    app_screen_set_brightness(3); /* 关屏期间改期望值（只缓存） */

    stub_reset();
    app_screen_oe_enable();

    CHECK_MSG(app_screen_is_oe_enabled(), "开屏后 is_oe_enabled 应为 true");
    CHECK_MSG(s_set_seen && s_set_level == 3,
              "开屏应恢复到期望值 3（不是关屏前的 5），实际施加 %u", (unsigned)s_set_level);
    CHECK_MSG(app_screen_get_brightness() == 3,
              "开屏后硬件亮度应为 3，实际 %u", (unsigned)app_screen_get_brightness());

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl), "开屏应置待下发（让从卡一起亮）");
    CHECK_MSG(lvl == 3, "开屏时待下发应为有效值 3，实际 %u", (unsigned)lvl);
}

/** @brief 重复关屏幂等：不改期望值、硬件仍 0；再开屏仍恢复到之前缓存的 3 */
static void case_repeat_off_idempotent(void)
{
    TEST_BEGIN("重复关屏幂等：期望值不变（3）、硬件仍 0、开屏仍恢复 3");

    brightness_reset();
    app_screen_set_brightness(5);
    app_screen_oe_disable();
    app_screen_set_brightness(3);
    stub_reset();

    app_screen_oe_disable();
    app_screen_oe_disable();

    CHECK_MSG(s_set_calls == 2,
              "重复关屏每次都应立即重新施加（2 次），实际 %d 次", s_set_calls);
    CHECK_MSG(s_set_level == 0, "重复关屏硬件应仍为 0，实际 %u", (unsigned)s_set_level);
    CHECK_MSG(s_bright_level == 3,
              "重复关屏不得改动期望值（应仍为 3），实际 %u", (unsigned)s_bright_level);
    CHECK_MSG(app_screen_get_brightness() == 0,
              "重复关屏后硬件亮度应为 0，实际 %u", (unsigned)app_screen_get_brightness());

    stub_reset();
    app_screen_oe_enable();
    CHECK_MSG(s_set_level == 3,
              "重复关屏后开屏仍应恢复缓存的 3，实际 %u", (unsigned)s_set_level);
}

/** @brief take_pending 取走一次后第二次返回 false（沿用既有语义，不得改成电平查询） */
static void case_take_pending_consumed_once(void)
{
    TEST_BEGIN("take_pending 只成功一次，且未取走时不改写输出参数");

    brightness_reset();
    app_screen_set_brightness(5);

    uint8_t lvl = 0xFF;
    CHECK_MSG(app_screen_brightness_take_pending(&lvl) && lvl == 5,
              "第一次取应返回 true 并给出 5，实际 %u", (unsigned)lvl);
    CHECK_MSG(!app_screen_brightness_take_pending(&lvl), "取走一次后第二次应返回 false");
    CHECK_MSG(lvl == 5, "第二次未取走时不应改写输出参数，实际 %u", (unsigned)lvl);
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m整屏门面 · 亮度与屏开关\033[0m\n");

    /* 本条检查的是 BSS 初值，**必须最先跑**，之前不得调用任何夹具。 */
    case_power_on_oe_enabled();
    case_set_brightness_applies_and_pends();
    case_brightness_clamped();
    case_oe_disable_gates_hardware();
    case_cache_retained_while_off();
    case_restore_on_oe_enable();
    case_repeat_off_idempotent();
    case_take_pending_consumed_once();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
