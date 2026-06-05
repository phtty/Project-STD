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

typedef struct dev_display dev_display_t;

/* ---- 操作虚表 ---- */
typedef struct dev_display_ops {
    void (*prepare)(dev_display_t *dev);           /* 像素→发送缓存 (dirty 时调用) */
    void (*scan)(dev_display_t *dev, uint8_t line); /* 输出一个扫描行 */
    void (*set_row)(uint8_t row);                  /* ABCD 行地址编码 */
    uint8_t scan_lines;                            /* 扫描行数 */
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

    /* 派生参数 */
    uint16_t screen_rows;         /* = modules_per_row * module_rows */
    uint16_t screen_cols;         /* = modules_per_col * module_cols */
    uint8_t  total_channels;      /* = modules_per_col * channels_per_module */
    uint16_t channel_pixels;      /* = module_rows * module_cols * modules_per_row / total_channels */
    uint16_t scan_line_pixels;    /* = channel_pixels / ops->scan_lines */
    uint16_t buffer_size;         /* = screen_rows * screen_cols */

    /* 缓冲区 (CCMRAM，派生实例静态分配) */
    uint8_t *pixel_map;
    uint8_t *hub75_buff;

    /* 运行时 */
    volatile uint8_t light_level;
    volatile bool    dirty;
};

/* ---- 通用 API ---- */

/** @brief 硬件初始化 (hw_dev_initcall): HUB75 引脚 + DBG 冻结 */
void dev_display_init(void);

/** @brief 软件初始化 (sw_dev_initcall): 创建 scan_task + 启动 TIM3/4 */
void dev_display_start(void);

/** @brief 设置单个像素颜色，置脏标记 */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, hub75_color_t color);

/** @brief 填充全屏，置脏标记 */
void dev_display_fill(dev_display_t *dev, hub75_color_t color);

/** @brief 获取 P20 模组显示实例 */
dev_display_t *dev_display_p20_get(void);

/** @brief 获取当前显示实例（向后兼容，等同 dev_display_p20_get） */
dev_display_t *dev_display_get(void);
