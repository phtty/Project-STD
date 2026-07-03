/**
 * @file    dev_key.c
 * @brief   按键/拨码开关设备实现 — EXTI 直接状态标记 + TEST 信号量
 */

#include "dev_key.h"

#include "main.h"
#include "initcall.h"
#include "cmsis_os2.h"
#include "pl_exti.h"

/* ---- 内部状态 ---- */
static volatile bool s_state[DEV_KEY_COUNT];
static osSemaphoreId_t s_test_sem;

/* ---- EXTI 回调（ISR 上下文，所有按键共用） ---- */
static void _exti_cb(uint16_t pin, void *ctx)
{
    (void)ctx;

    if (pin == SW1_Pin)
        s_state[DEV_KEY_SW1] = true;
    else if (pin == SW2_Pin)
        s_state[DEV_KEY_SW2] = true;
    else if (pin == SW3_Pin)
        s_state[DEV_KEY_SW3] = true;
    else if (pin == KEY_TST_Pin && s_test_sem)
        osSemaphoreRelease(s_test_sem);
}

/* ---- 硬件初始化：注册 EXTI 回调 ---- */
void dev_key_init(void)
{
    pl_exti_register_cb(SW1_Pin, _exti_cb, NULL);
    pl_exti_register_cb(SW2_Pin, _exti_cb, NULL);
    pl_exti_register_cb(SW3_Pin, _exti_cb, NULL);
    pl_exti_register_cb(KEY_TST_Pin, _exti_cb, NULL);
}
hw_dev_initcall(dev_key_init);

/* ---- 软件初始化：创建 TEST 信号量 ---- */
void dev_key_sw_init(void)
{
    s_test_sem = osSemaphoreNew(1, 0, NULL);
}
sw_dev_initcall(dev_key_sw_init);

/* ---- 公开 API ---- */

bool dev_key_get_state(dev_key_id_t key_id)
{
    if (key_id >= DEV_KEY_COUNT) return false;
    return s_state[key_id];
}

void dev_key_clear_state(dev_key_id_t key_id)
{
    if (key_id < DEV_KEY_COUNT)
        s_state[key_id] = false;
}

bool dev_key_test_pressed(void)
{
    if (!s_test_sem) return false;
    return (osSemaphoreAcquire(s_test_sem, 0) == osOK);
}
