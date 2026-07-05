/**
 * @file    app_key.h
 * @brief   按键应用层 — dev_key 的薄封装
 *
 * SW1~SW3/TEST: dev_key_wait_press 信号量驱动，零轮询
 * DIP1~DIP2:   上电时读取一次即可
 */

#pragma once

#include <stdbool.h>
#include "dev_key.h"

/** @brief 获取 DIP 拨码上电缓存值（SW/TEST 请直接用 dev_key API） */
bool app_key_get_state(dev_key_id_t key_id);
