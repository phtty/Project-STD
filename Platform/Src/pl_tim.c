/**
 * @file        pl_tim.c
 * @brief       定时器平台层抽象
 *
 * 本文件只有机制，不含任何"哪几个定时器存在"的事实——那些来自板级的
 * g_pl_tim_board[]（见 boards/<板>/Src/pl_tim_board.c），因此本文件跨板共享。
 */

#include "pl_tim.h"
#include "initcall.h"
#include "stm32f4xx_hal.h"

static pl_tim_handle_t g_tim_handle[PL_TIM_MAX];
static pl_tim_period_cb_t g_period_cb[PL_TIM_MAX];

/* ---- 回调注册 ---- */
void pl_tim_register_period_cb(uint8_t tim_id, pl_tim_period_cb_t cb)
{
    if (tim_id < PL_TIM_MAX) g_period_cb[tim_id] = cb;
}

/* ---- 初始化 ---- */
void pl_tim_init(void)
{
    for (uint8_t i = 0; i < PL_TIM_MAX; i++) {
        if (g_pl_tim_board[i].init) g_pl_tim_board[i].init();
        g_tim_handle[i] = g_pl_tim_board[i].handle;
    }
}
hw_pl_initcall(pl_tim_init);

/* ---- 公开 API ---- */
pl_tim_handle_t pl_tim_get_handle(uint8_t id)
{
    return (id < PL_TIM_MAX) ? g_tim_handle[id] : NULL;
}

uint8_t pl_tim_irq_of(uint8_t id)
{
    return (id < PL_TIM_MAX) ? g_pl_tim_board[id].irq : 0;
}

void pl_tim_start_it(pl_tim_handle_t h)
{
    if (h)
        HAL_TIM_Base_Start_IT((TIM_HandleTypeDef *)h);
}

void pl_tim_irq_disable(uint8_t irq)
{
    NVIC_DisableIRQ(irq);
}
void pl_tim_irq_enable(uint8_t irq)
{
    NVIC_EnableIRQ(irq);
}

void pl_tim_dbg_freeze(pl_tim_handle_t h)
{
    if (!h) return;

    /* 用 if 链而非 switch：TIMx 展开成 ((TIM_TypeDef *)TIMx_BASE)，是指针类型，
       不是整型常量表达式，不能做 case 标签（clang 直接拒绝）。 */
    TIM_TypeDef *inst = ((TIM_HandleTypeDef *)h)->Instance;
    if (inst == TIM2)      __HAL_DBGMCU_FREEZE_TIM2();
    else if (inst == TIM3) __HAL_DBGMCU_FREEZE_TIM3();
    else if (inst == TIM4) __HAL_DBGMCU_FREEZE_TIM4();
    else if (inst == TIM5) __HAL_DBGMCU_FREEZE_TIM5();
    else if (inst == TIM6) __HAL_DBGMCU_FREEZE_TIM6();
    else if (inst == TIM7) __HAL_DBGMCU_FREEZE_TIM7();
}

/* ---- HAL 周期回调（分派到注册的模块回调）----
 * TIM7 是 HAL 时基：不查表直接喂 tick，本板有没有 TIM7 由 HAL 时基配置决定。 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM7) {
        HAL_IncTick();
        return;
    }
    for (uint8_t i = 0; i < PL_TIM_MAX; i++)
        if (g_tim_handle[i] == htim && g_period_cb[i]) {
            g_period_cb[i]();
            return;
        }
}

/* ---- ISR ----
 * 表驱动：本板没有的定时器句柄为 NULL，对应 ISR 不会真正触发（NVIC 没使能），
 * 这里的判空只是防御。中断向量必须存在于所有板上，所以 ISR 留在共享文件里。 */
#define PL_TIM_ISR(n)                            \
    void TIM##n##_IRQHandler(void)               \
    {                                            \
        if (g_tim_handle[PL_TIM##n])             \
            HAL_TIM_IRQHandler(g_tim_handle[PL_TIM##n]); \
    }

PL_TIM_ISR(2)
PL_TIM_ISR(3)
PL_TIM_ISR(4)
PL_TIM_ISR(7)
