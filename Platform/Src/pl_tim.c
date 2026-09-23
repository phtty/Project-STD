/**
 * @file        pl_tim.c
 * @brief       定时器平台层抽象
 *
 * 本文件只有机制，不含任何"哪几个定时器存在"的事实——那些来自板级的
 * g_pl_tim_board[]（见 boards/&lt;板&gt;/Platform/Src/pl_tim_board.c），因此本文件跨板共享。
 */

#include "pl_tim.h"
#include "initcall.h"
#include "stm32f4xx_hal.h"

/* 刻意**不设** g_tim_handle[] 这类"运行时句柄数组"。板级表 g_pl_tim_board[] 是
 * const、编译期初始化，从复位第一条指令起就有效；而任何在 initcall 里填充的数组，
 * 在 initcall 之前都是空的 —— 中断源却可能在那之前就已经使能。
 *
 * 这不是假设：TIM7 是 HAL 时基，由 HAL_Init() → HAL_InitTick() 启动，**早于
 * initcall_run()**。本文件曾有一版把 ISR 写成 `if (g_tim_handle[PL_TIM7])
 * HAL_TIM_IRQHandler(...)`，于是第一次 TIM7 触发时句柄还是 NULL、ISR 什么都不做、
 * 更新标志永不清除，中断立刻重入 —— CPU 再也没出来，pl_tim_init 根本没机会跑。
 * （症状："卡死在 TIM7 中断里"。已上机复现。）
 *
 * 结论：ISR 与任何可能在初始化完成前被调用的路径，一律只读编译期常量。 */
static pl_tim_period_fn_t s_period_cb[PL_TIM_MAX];

/* ---- 回调注册 ---- */
void pl_tim_register_period_cb(uint8_t tim_id, pl_tim_period_fn_t cb)
{
    if (tim_id < PL_TIM_MAX) s_period_cb[tim_id] = cb;
}

/* ---- 初始化 ---- */
void pl_tim_init(void)
{
    for (uint8_t i = 0; i < PL_TIM_MAX; i++)
        if (g_pl_tim_board[i].init) g_pl_tim_board[i].init();
}
hw_pl_initcall(pl_tim_init);

/* ---- 公开 API ---- */
pl_tim_handle_t pl_tim_get_handle(uint8_t id)
{
    /* 直接查板级表：即使 pl_tim_init 还没跑（外设尚未配置），返回的句柄指针也是
       有效的 —— 调用方本来就要自己保证不在初始化前用。 */
    return (id < PL_TIM_MAX) ? g_pl_tim_board[id].handle : NULL;
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
        if (g_pl_tim_board[i].handle == (pl_tim_handle_t)htim && s_period_cb[i]) {
            s_period_cb[i]();
            return;
        }
}

/* ---- ISR ----
 * 取句柄一律走**板级常量表**，绝不走运行时数组 —— 见文件上方那段说明：
 * TIM7 由 HAL_Init 启动，早于任何 initcall，ISR 里的判空一旦依赖初始化期数据
 * 就会变成中断风暴。
 *
 * 判空本身仍然保留：本板没有的定时器句柄为 NULL（NVIC 也没使能，理论上不会进来），
 * 但万一来了一条假中断，宁可什么都不做也好过拿 NULL 去解引用。 */
#define PL_TIM_ISR(n)                                                          \
    void TIM##n##_IRQHandler(void)                                             \
    {                                                                          \
        TIM_HandleTypeDef *h = (TIM_HandleTypeDef *)g_pl_tim_board[PL_TIM##n].handle; \
        if (h) HAL_TIM_IRQHandler(h);                                          \
    }

PL_TIM_ISR(2)
PL_TIM_ISR(3)
PL_TIM_ISR(4)
PL_TIM_ISR(7)
