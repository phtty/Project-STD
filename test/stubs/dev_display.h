/**
 * @file    dev_display.h
 * @brief   host 单测用的 dev_display.h 替身
 *
 * app_render.h / dev_light_sensor.h 只需要这里的两样东西：颜色枚举与显示几何。
 * 真正的 dev_display.h 会拉进 pl_hub75.h → main.h → HAL，host 编不了。
 *
 * 这里是**有意复制的定义**：若真头文件新增颜色，用到它的测试会编译失败（报错
 * 而非静默偏差），这是可接受的失败方式。
 *
 * ---- 几何数值必须与实机一致 ----
 * 与参考工程不同，本工程的真 dev_display.h **没有**编译期几何宏 —— 屏幕尺寸是
 * 运行时的 dev->screen_rows / dev->screen_cols，由所选模组实例决定。下面两个宏
 * 只是 host 侧的等价常量（保持与参考工程同名，便于将来移植用例），**不表示真
 * 头文件里有它们**：如果测试需要的是"实机几何"，请用运行时值，别依赖这两个宏。
 *
 * 数值来源：本工程当前编译进固件的模组是 Device/Display/dev_p20_16x8_2200001667.c
 * （Makefile 的 SRC_DEVICE 只列了这一个），其实例初始化把 screen_rows/screen_cols
 * 填成：
 *     P20_SCREEN_ROWS = P20_MODULE_ROWS(8) * P20_MODULE_PIXEL_ROW(16) = 128
 *     P20_SCREEN_COLS = P20_MODULE_COLS(4) * P20_MODULE_PIXEL_COL(8)  = 32
 * **该文件里宏定义旁边的过期注释不要照抄**（例如把 SCREEN_ROWS 注释成 16，
 *   实际是 8 × 16 = 128）—— 以 `.screen_rows =` 的实际赋值为准（本工程是 128 × 32）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* 与 Device/Inc/dev_display.h 保持一致 */
typedef enum {
    COLOR_BLACK  = 0,
    COLOR_RED    = 1,
    COLOR_GREEN  = 2,
    COLOR_YELLOW = 3,
    COLOR_BLUE   = 4,
    COLOR_PURPLE = 5,
    COLOR_CYAN   = 6,
    COLOR_WHITE  = 7,
} display_color_t;

#define DEV_DISPLAY_SCREEN_ROWS (128U)
#define DEV_DISPLAY_SCREEN_COLS (32U)
