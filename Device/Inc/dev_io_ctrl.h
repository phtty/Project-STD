/**
 * @file    dev_io_ctrl.h
 * @brief   IO 控制设备（车道灯、闪光灯）
 */

#pragma once

#include <stdbool.h>

void dev_io_lane_light(bool enable);
void dev_io_flash_light(bool enable);
