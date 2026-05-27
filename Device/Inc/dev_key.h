/**
 * @file    dev_key.h
 * @brief   按键/拨码开关设备
 */

#pragma once

#include <stdint.h>

typedef void (*dev_key_cb_t)(uint8_t key_id);

void dev_key_init(void);
void dev_key_register_cb(uint8_t key_id, dev_key_cb_t cb);
