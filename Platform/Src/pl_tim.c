/**
 * @file        pl_tim.c
 * @brief       定时器平台层抽象
 *
 * 封装 TIM2/3/4/7 的初始化、启动和 ISR。
 */

#include "pl_tim.h"
#include "tim.h"
#include "initcall.h"

extern TIM_HandleTypeDef htim7;

static TIM_HandleTypeDef *g_tim_handle[PL_TIM_MAX];
static pl_tim_period_cb_t g_period_cb[PL_TIM_MAX];

/* ---- 回调注册 ---- */
void pl_tim_register_period_cb(uint8_t tim_id, pl_tim_period_cb_t cb)
{
    if (tim_id < PL_TIM_MAX) g_period_cb[tim_id] = cb;
}

/* ---- 初始化 ---- */
void pl_tim_init(void)
{
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_TIM4_Init();

    g_tim_handle[PL_TIM2] = &htim2;
    g_tim_handle[PL_TIM3] = &htim3;
    g_tim_handle[PL_TIM4] = &htim4;
    g_tim_handle[PL_TIM7] = &htim7;
}
hw_pl_initcall(pl_tim_init);

/* ---- 公开 API ---- */
pl_tim_handle_t pl_tim_get_handle(uint8_t id)
{
    return (id < PL_TIM_MAX) ? g_tim_handle[id] : NULL;
}

void pl_tim_start_it(pl_tim_handle_t h)
{
    if (h) HAL_TIM_Base_Start_IT((TIM_HandleTypeDef *)h);
}

void pl_tim_irq_disable(uint8_t irq)
{ NVIC_DisableIRQ(irq); }
void pl_tim_irq_enable(uint8_t irq)
{ NVIC_EnableIRQ(irq); }

void pl_tim_dbg_freeze(pl_tim_handle_t h)
{
    if (h && ((TIM_HandleTypeDef *)h)->Instance == TIM3)
        __HAL_DBGMCU_FREEZE_TIM3();
}

/* ---- HAL 周期回调（分派到注册的模块回调）---- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM7) { HAL_IncTick(); return; }
    for (uint8_t i = 0; i < PL_TIM_MAX; i++)
        if ((TIM_HandleTypeDef *)g_tim_handle[i] == htim && g_period_cb[i])
            { g_period_cb[i](); return; }
}

/* ---- ISR ---- */
void TIM2_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim2);
}
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}
void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim4);
}
void TIM7_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim7);
}
