/**
 * @file    dev_display.h
 * @brief   HUB75 LED 点阵显示设备
 *
 * 封装像素缓冲区、扫描引擎和亮度控制。
 * pixel_map/hub75_buff 在 CCMRAM 中静态分配。
 */

#pragma once

#include <stdint.h>
#include "pl_hub75.h"

/* ---- 显示参数 ---- */
#define MODULE_PER_ROW       8
#define MODULE_PER_COL       4
#define MODULE_PIXEL_ROW     16
#define MODULE_PIXEL_COL     16
#define MODULE_CHANNEL_NUM   2
#define MODULE_SCAN_LINE_NUM 1
#define GROUP_SIZE           16

#define SCREEN_PIXEL_ROW     (MODULE_PER_ROW * MODULE_PIXEL_ROW)
#define SCREEN_PIXEL_COL     (MODULE_PER_COL * MODULE_PIXEL_COL)
#define DISRAM_SIZE          (SCREEN_PIXEL_ROW * SCREEN_PIXEL_COL)
#define CHANNEL_NUM          (MODULE_PER_COL * MODULE_CHANNEL_NUM)
#define CHANNEL_PIXEL_NUM    (MODULE_PIXEL_ROW * MODULE_PIXEL_COL * MODULE_PER_ROW / MODULE_CHANNEL_NUM)
#define SCAN_LINE_PIXEL_NUM  (CHANNEL_PIXEL_NUM / MODULE_SCAN_LINE_NUM)

/* ---- 显示设备 ---- */
typedef struct {
    uint8_t *pixel_map;     /* 像素帧缓冲（CCMRAM） */
    uint8_t *hub75_buff;    /* HUB75 扫描缓冲（CCMRAM） */
    volatile uint8_t light_level; /* 亮度等级 0-7 */
} display_dev_t;

/** @brief 初始化显示设备（分配 CCMRAM 缓冲区、启动 TIM3/TIM4） */
void dev_display_init(display_dev_t *dev);

/** @brief 设置单个像素颜色 */
void dev_display_set_pixel(display_dev_t *dev, uint16_t x, uint16_t y, hub75_color_t color);

/** @brief 填充全屏 */
void dev_display_fill(display_dev_t *dev, hub75_color_t color);

/** @brief 像素图转换（pixel_map → hub75_buff） */
void dev_display_convert(display_dev_t *dev);

/** @brief TIM3 ISR：输出 HUB75 扫描行 */
void dev_display_tim3_isr(display_dev_t *dev);

/** @brief TIM4 ISR：PWM 亮度控制 */
void dev_display_tim4_isr(display_dev_t *dev);

/** @brief 获取全局显示设备实例 */
display_dev_t *dev_display_get(void);
