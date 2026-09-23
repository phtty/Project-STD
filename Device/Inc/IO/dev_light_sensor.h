/**
 * @file    dev_light_sensor.h
 * @brief   环境光传感器设备（ADC 采样 + 自动亮度调节）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dev_display.h"

/** @brief 环境光传感器设备实例（由调用方分配并持久化） */
typedef struct {
    void *adc;                         /**< ADC 句柄（init 时由 pl_adc_get_handle 填充） */
    dev_display_t *display;            /**< 关联的显示设备，亮度写往它 */
    volatile bool auto_adjust_enabled; /**< false = 停止跟随, 亮度由外部指定 */

    /* 亮度应用回调。**非 NULL 时由它套用等级**，NULL 则直接写 display->light_level。
     *
     *  为什么要留这条缝：本模块属 Device 层，不能反向依赖 Application。而亮度在
     *  级联场景下要经 app_screen 统一下发给所有卡 —— 由 Device 层直接写
     *  display->light_level 的话，从卡永远收不到主卡的调光。 */
    void (*apply)(void *ctx, uint8_t level); /**< 亮度应用回调；NULL 则直接写 display */
    void *apply_ctx;                   /**< 传给 apply 的上下文 */
} dev_light_sensor_dev_t;

/** @brief 初始化光传感器设备
 *  @param[out] dev    待填充的设备实例；写入 adc/display/auto_adjust_enabled
 *  @param display     关联的显示设备 */
void dev_light_sensor_init(dev_light_sensor_dev_t *dev, dev_display_t *display);

/** @brief 采样一次环境光并换算成亮度等级
 *  @param dev 设备实例
 *  @return 亮度等级 1~7（0 保留给关屏，本函数不返回） */
uint8_t dev_light_sensor_read(dev_light_sensor_dev_t *dev);

/** @brief 按当前光照自动调节亮度（带连续 N 次抗抖动）
 *  @param dev 设备实例 */
void dev_light_sensor_auto_adjust(dev_light_sensor_dev_t *dev);
