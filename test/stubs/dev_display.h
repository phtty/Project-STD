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
 * 数值来源：固件编入哪块屏由 `boards/<板>/board.mk` 的 SRC_BOARD 决定（**只列一个**）。
 * 同一块 3833024 可以换面板，几何因此是**构建期事实**、不是常量：
 *     dev_p20_16x8_2200001667.c（P20 16×8）  → 8×16 = **128** 宽，4×8 = **32** 高
 *     dev_p10_32x16_2200001703.c（P10 32×16）→ 10×32 = **320** 宽，4×16 = **64** 高
 * **本分支当前编的是 P20（128×32）**；320×64 的 P10 驱动也在同一个目录，但换用前
 * 必须先确认面板扫描方式（换过一版、冒烟上半屏偶发不亮，已回退，见 docs/测试约定.md §10 O8）。
 * **那些驱动里宏定义旁边的过期注释不要照抄**（例如把 SCREEN_ROWS 注释成 16，
 *   实际是 8 × 16 = 128）—— 以 `.screen_rows =` 的实际赋值为准。
 *
 * 下面两个宏只是 host 侧"默认板带 P20 屏"的等价常量；要测 320×64（P10），
 * 用例自己给假实例的几何（见 test_screen_canvas / test_cq_p10）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* 与 Device/Inc/dev_display.h 保持一致 */
typedef enum {
    DEV_DISPLAY_COLOR_BLACK  = 0,
    DEV_DISPLAY_COLOR_RED    = 1,
    DEV_DISPLAY_COLOR_GREEN  = 2,
    DEV_DISPLAY_COLOR_YELLOW = 3,
    DEV_DISPLAY_COLOR_BLUE   = 4,
    DEV_DISPLAY_COLOR_PURPLE = 5,
    DEV_DISPLAY_COLOR_CYAN   = 6,
    DEV_DISPLAY_COLOR_WHITE  = 7,
} dev_display_color_t;

/* 与真头一致的能力掩码宏（本轮无消费方，仅为影子结构体保持同步）*/
#define DEV_DISPLAY_COLOR_ALL (0xFEU)
#define DEV_DISPLAY_COLOR_BIT(c) ((uint8_t)(1U << (c)))

#define DEV_DISPLAY_SCREEN_ROWS (128U)
#define DEV_DISPLAY_SCREEN_COLS (32U)

/* ---- 结构体与像素原语 ----
 *
 * 真 dev_display.h 里这个结构体有 ops 虚表、模组参数等一堆字段，且会拉进
 * pl_hub75.h → main.h → HAL，host 编不了。这里只留**画布用例真正用到的**那几个：
 * 几何、帧缓冲、亮度、脏标记。ops 用 void* 占位，免得为它再定义一套虚表类型。
 *
 * 原语的**实现不在这里**，由各测试自己给 —— 画布用例要拿"直写实屏"当独立参考，
 * 自己的实现才好控制；链接真的 dev_display.c 反而要拖进 pl_tim/pl_task 一整串。 */
typedef struct dev_display dev_display_t; /**< 前置声明：ops 里要用到它 */

/** @brief 显示操作虚表（与真头同形：模组驱动注入 prepare/scan/set_row）
 *
 *  板级模组驱动要能在 host 上原样编译，所以这里必须给全三支 —— 只测画布的套件
 *  用不到它，但用了它（`test_display_p10`）的套件就是靠这份影子结构体跑真驱动。 */
typedef struct dev_display_ops {
    void (*prepare)(dev_display_t *dev);
    void (*scan)(dev_display_t *dev, uint8_t line);
    void (*set_row)(uint8_t row);
} dev_display_ops_t;

struct dev_display {
    const dev_display_ops_t *ops; /* 首成员：与真头一致 */
    /* ---- 模组参数（顺序与真头一致）---- */
    uint8_t  module_rows;
    uint8_t  module_cols;
    uint8_t  channels_per_module;
    uint8_t  modules_per_row;
    uint8_t  modules_per_col;
    uint8_t  scan_lines;
    uint8_t  supported_color_mask;
    /* ---- 派生参数 ---- */
    uint16_t screen_rows; /* 宽（注意：本工程 screen_rows 是宽、screen_cols 是高） */
    uint16_t screen_cols; /* 高 */
    uint8_t  total_channels;
    uint16_t channel_pixels;
    uint16_t scan_line_pixels;
    uint16_t buffer_size;
    /* ---- 缓冲 ---- */
    uint8_t *pixel_map;
    uint8_t *hub75_buff;
    /* ---- 运行态 ---- */
    uint8_t  light_level;
    bool     dirty;
    bool     dirty_hold;    /* 与真头一致：见 dev_display_frame_begin */
    bool     frame_touched;
};

/** @brief 注册当前生效的显示模组（真实现是"后来者覆盖"，见 dev_display.c） */
void dev_display_register(dev_display_t *dev);

void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color);
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      dev_display_color_t color);
void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, dev_display_color_t color);
void dev_display_set_brightness(dev_display_t *dev, uint8_t level);
void dev_display_frame_begin(dev_display_t *dev);
void dev_display_frame_end(dev_display_t *dev);
dev_display_t *dev_display_get(void);

/* 与真头同步的能力查询声明（本轮无消费方；测试若调用需自备桩实现）*/
bool dev_display_supports_color(const dev_display_t *dev, dev_display_color_t c);
uint8_t dev_display_color_mask(const dev_display_t *dev);
