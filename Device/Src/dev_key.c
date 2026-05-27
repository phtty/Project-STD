#include "main.h"
/**
 * @file    dev_key.c
 * @brief   按键/拨码开关设备实现（依赖 pl_exti 平台抽象）
 */

#include "dev_key.h"
#include "pl_exti.h"
#include <stddef.h>

static dev_key_cb_t s_key_cb[8];

static void key_exti_cb(uint16_t pin, void *ctx)
{
    (void)ctx;
    uint8_t id = 0;
    if (pin == SW1_Pin)      id = 0;
    else if (pin == SW2_Pin) id = 1;
    else if (pin == SW3_Pin) id = 2;
    else return;
    if (s_key_cb[id]) s_key_cb[id](id);
}

void dev_key_init(void)
{
    for (int i = 0; i < 8; i++) s_key_cb[i] = NULL;
    pl_exti_register_cb(SW1_Pin, key_exti_cb, NULL);
    pl_exti_register_cb(SW2_Pin, key_exti_cb, NULL);
    pl_exti_register_cb(SW3_Pin, key_exti_cb, NULL);
}

void dev_key_register_cb(uint8_t key_id, dev_key_cb_t cb)
{
    if (key_id < 8) s_key_cb[key_id] = cb;
}
