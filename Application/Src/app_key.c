/**
 * @file    app_key.c
 * @brief   按键应用层 — dev_key 薄封装
 */

#include "app_key.h"

#include "initcall.h"

static bool s_dip1_state;
static bool s_dip2_state;

bool app_key_get_state(dev_key_id_t key_id)
{
    switch (key_id) {
        case DEV_KEY_DIP1: return s_dip1_state;
        case DEV_KEY_DIP2: return s_dip2_state;
        default:           return dev_key_get_state(key_id);
    }
}

/* ---- 上电读取 DIP ---- */
static void _app_key_init(void)
{
    s_dip1_state = dev_key_get_state(DEV_KEY_DIP1);
    s_dip2_state = dev_key_get_state(DEV_KEY_DIP2);
}
sw_app_initcall(_app_key_init);
