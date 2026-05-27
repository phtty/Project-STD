/**
 * @file    pl_exti.c
 * @brief   EXTI 外部中断 Platform 层抽象
 *
 * ISR 收敛于此，Device 层通过 pl_exti_register_cb 注册回调。
 */

#include "pl_exti.h"
#include "main.h"
#include "initcall.h"

#define PL_EXTI_CB_MAX 16

typedef struct {
    uint16_t pin;
    pl_exti_cb_t cb;
    void *ctx;
} exti_entry_t;

static exti_entry_t s_cb[PL_EXTI_CB_MAX];

void pl_exti_init(void)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++)
        s_cb[i].pin = 0, s_cb[i].cb = NULL;
}
hw_device_initcall(pl_exti_init);

void pl_exti_register_cb(uint16_t pin, pl_exti_cb_t cb, void *ctx)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++) {
        if (s_cb[i].cb == NULL) {
            s_cb[i].pin  = pin;
            s_cb[i].cb   = cb;
            s_cb[i].ctx  = ctx;
            return;
        }
    }
}

/* ---- ISR（从 stm32f4xx_it.c 迁移） ---- */
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(KEY_TST_Pin);
}

void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(SW3_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW2_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW1_Pin);
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++)
        if (s_cb[i].cb && s_cb[i].pin == pin)
            s_cb[i].cb(pin, s_cb[i].ctx);
}
