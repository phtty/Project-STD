/**
 * @file    pl_exti.c
 * @brief   EXTI 外部中断 Platform 层抽象
 *
 * Device 层通过 pl_exti_register_fn 注册回调；本文件只提供 ISR→回调分派，
 * 不持有应用级 RTOS 对象。
 *
 * 中断向量不在这里：哪些 EXTI 向量存在、各自服务哪几根引脚是板级事实，
 * 写在 boards/&lt;板&gt;/Platform/Src/pl_exti_board.c，函数体复用 HAL_GPIO_EXTI_IRQHandler +
 * 本文件的 HAL_GPIO_EXTI_Callback。
 */

#include "pl_exti.h"
#include "initcall.h"
#include <stddef.h> /* NULL（原经 main.h 间接引入，去掉板级头后要显式带） */

#define PL_EXTI_FN_MAX 16

typedef struct {
    uint16_t pin;
    pl_exti_fn_t fn;
    void *ctx;
} exti_entry_t;

static exti_entry_t s_exti_fn_table[PL_EXTI_FN_MAX];

void pl_exti_init(void)
{
    for (int i = 0; i < PL_EXTI_FN_MAX; i++)
        s_exti_fn_table[i].pin = 0, s_exti_fn_table[i].fn = NULL;
}
hw_pl_initcall(pl_exti_init);

void pl_exti_register_fn(uint16_t pin, pl_exti_fn_t fn, void *ctx)
{
    for (int i = 0; i < PL_EXTI_FN_MAX; i++) {
        if (s_exti_fn_table[i].fn == NULL) {
            s_exti_fn_table[i].pin  = pin;
            s_exti_fn_table[i].fn   = fn;
            s_exti_fn_table[i].ctx  = ctx;
            return;
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    for (int i = 0; i < PL_EXTI_FN_MAX; i++)
        if (s_exti_fn_table[i].fn && s_exti_fn_table[i].pin == pin)
            s_exti_fn_table[i].fn(pin, s_exti_fn_table[i].ctx);
}
