/**
 * @file    dev_display.h
 * @brief   HUB75 LED 点阵显示设备 — OCP 虚表基类
 *
 * 基类提供通用参数和 scan_task 调度骨架。
 * 派生类通过 ops 虚表注入模组差异：像素映射、行地址编码、扫描策略。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pl_hub75.h"

/* ---- 颜色（上层 API 使用，不与 HUB75 引脚耦合）---- */
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

typedef struct dev_display dev_display_t;

/* ---- 操作虚表 ---- */
typedef struct dev_display_ops {
    void (*prepare)(dev_display_t *dev);           /* 像素→发送缓存 (dirty 时调用) */
    void (*scan)(dev_display_t *dev, uint8_t line); /* 输出一个扫描行 */
    void (*set_row)(uint8_t row);                  /* ABCD 行地址编码 */
} dev_display_ops_t;

/* ---- 基类 (派生类必须将其放在第一个成员位置) ---- */
struct dev_display {
    const dev_display_ops_t *ops;  /* 第一个成员 */

    /* 通用参数 */
    uint8_t  module_rows;         /* 单模块像素行数 */
    uint8_t  module_cols;         /* 单模块像素列数 */
    uint8_t  channels_per_module; /* 每模块通道数 */
    uint8_t  modules_per_row;     /* 每行模块数 */
    uint8_t  modules_per_col;     /* 每列模块数 */
    uint8_t  scan_lines;          /* 扫描行数 (静态=1, 1/4扫=4...) */

    /* 派生参数 */
    uint16_t screen_rows;         /* = modules_per_row * module_rows */
    uint16_t screen_cols;         /* = modules_per_col * module_cols */
    uint8_t  total_channels;      /* = modules_per_col * channels_per_module */
    uint16_t channel_pixels;      /* = module_rows * module_cols * modules_per_row / total_channels */
    uint16_t scan_line_pixels;    /* = channel_pixels / scan_lines */
    uint16_t buffer_size;         /* = screen_rows * screen_cols */

    /* 缓冲区 (CCMRAM，派生实例静态分配) */
    uint8_t *pixel_map;
    uint8_t *hub75_buff;

    /* 运行时 */
    volatile uint8_t light_level;
    volatile bool    dirty;
    /** 见 `dev_display_frame_begin()`：这一段绘制当成一帧，期间不置脏标记 */
    bool dirty_hold;
    /** 压制期间**真的写过实屏缓冲**没有 —— `frame_end` 据此决定要不要输出
     *  （画布路径整段都不碰实屏缓冲，那一段就不该触发 prepare） */
    bool frame_touched;
};

/* ---- 通用 API ---- */

/** @brief 硬件初始化 (hw_dev_initcall): HUB75 引脚 + DBG 冻结 */
void dev_display_init(void);

/** @brief 软件初始化 (sw_dev_initcall): 创建 scan_task + 启动 TIM3/4 */
void dev_display_start(void);

/** @brief 设置单个像素颜色，置脏标记 */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, display_color_t color);

/** @brief 矩形区域填充纯色: (x,y)起点, w宽h高, 超出屏幕自动截断, 置脏标记 */
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t color);

/** @brief 叠加绘制位图: (x,y)起点, w宽h高, bitmap每行( (w+7)/8 )字节, bit=1写color, bit=0不改变原像素。
 *  如需不透明绘制(bit=0置黑), 调用方先 dev_display_fill 填充背景色 */
void dev_display_draw_bitmap(dev_display_t *dev,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h,
    const uint8_t *bitmap, display_color_t color);

/** @brief 获取 P20 模组显示实例 */
dev_display_t *dev_display_p20_get(void);

/** @brief 注册活动显示实例（由显示模组的 hw_dev_initcall 调用） */
void dev_display_register(dev_display_t *dev);

/** @brief 获取当前活动显示实例 */
dev_display_t *dev_display_get(void);

/** @brief 设置亮度 (0=最暗/关闭, 7=最亮)，PWM 粒度 1/8 */
void dev_display_set_brightness(dev_display_t *dev, uint8_t level);

/** @brief 把接下来的若干次绘制**当成一帧**输出（期间不置脏标记，`_end` 时置一次）
 *
 *  为什么需要它：多步更新（清背景 + 画内容）之间，`scan_task` 是
 *  `osPriorityRealtime`，完全可能在两步之间跑一次 `prepare` —— 屏上就闪出中间态
 *  （一帧全黑、或半张新半张旧）。压住脏标记之后，屏上只出现最终那一帧。
 *
 *  **画布路径**（多卡主卡，渲染目标是 1bpp 画布）整段都不碰实屏缓冲 —— 那种情况下
 *  `_end` 不会置脏标记（`frame_touched` 记着），不会白跑一次 prepare。
 *
 *  **必须成对**：忘了 `_end` 的表现是"这一段更新一直不上屏"，不报错。
 *  与落屏路径原来那句 `dev->dirty = false` 是同一件事，这里提成显式接口 ——
 *  上层不必再去摸 `dirty`。 */
void dev_display_frame_begin(dev_display_t *dev);
void dev_display_frame_end(dev_display_t *dev);
