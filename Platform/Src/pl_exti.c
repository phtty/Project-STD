/**
 * @file    pl_exti.c
 * @brief   EXTI 外部中断 Platform 层抽象
 *
 * Device 层通过 pl_exti_register_cb 注册回调；本文件只提供 ISR→回调分派，
 * 不持有应用级 RTOS 对象。
 *
 * 中断向量不在这里：哪些 EXTI 向量存在、各自服务哪几根引脚是板级事实，
 * 写在 boards/<板>/Src/pl_exti_board.c，函数体复用 HAL_GPIO_EXTI_IRQHandler +
 * 本文件的 HAL_GPIO_EXTI_Callback。
 */

#include "pl_exti.h"
#include "initcall.h"
#include <stddef.h> /* NULL（原经 main.h 间接引入，去掉板级头后要显式带） */

#define PL_EXTI_CB_MAX 16

typedef struct {
    uint16_t pin;
    pl_exti_fn_t cb;
    void *ctx;
} exti_entry_t;

static exti_entry_t s_exti_cb_table[PL_EXTI_CB_MAX];

void pl_exti_init(void)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++)
        s_exti_cb_table[i].pin = 0, s_exti_cb_table[i].cb = NULL;
}
hw_pl_initcall(pl_exti_init);

void pl_exti_register_cb(uint16_t pin, pl_exti_fn_t cb, void *ctx)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++) {
        if (s_exti_cb_table[i].cb == NULL) {
            s_exti_cb_table[i].pin  = pin;
            s_exti_cb_table[i].cb   = cb;
            s_exti_cb_table[i].ctx  = ctx;
            return;
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    for (int i = 0; i < PL_EXTI_CB_MAX; i++)
        if (s_exti_cb_table[i].cb && s_exti_cb_table[i].pin == pin)
            s_exti_cb_table[i].cb(pin, s_exti_cb_table[i].ctx);
}
