/**
 * @file    dev_key.c
 * @brief   按键/拨码开关设备实现
 */

#include "dev_key.h"
#include <stddef.h>

static dev_key_cb_t s_key_cb[8];

void dev_key_init(void)
{
    for (int i = 0; i < 8; i++) s_key_cb[i] = NULL;
}

void dev_key_register_cb(uint8_t key_id, dev_key_cb_t cb)
{
    if (key_id < 8) s_key_cb[key_id] = cb;
}
