/**
 * @file    dev_io_ctrl.h
 * @brief   IO 控制设备（车道灯、闪光灯）
 */

#pragma once

#include <stdbool.h>

/** @brief 初始化 IO 控制设备（车道灯/闪光灯，初始关闭） */
void dev_io_ctrl_init(void);

/** @brief 控制车道灯
 *  @param enable  true = 点亮，false = 熄灭 */
void dev_io_lane_light(bool enable);

/** @brief 控制闪光灯
 *  @param enable  true = 点亮，false = 熄灭 */
void dev_io_flash_light(bool enable);
