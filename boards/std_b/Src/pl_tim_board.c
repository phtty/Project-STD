/**
 * @file    pl_tim_board.c
 * @brief   std_b 的定时器板级表
 *
 * 共享的 Platform/Src/pl_tim.c 只认 g_pl_tim_board[]，本文件是它对这块板的答案。
 *
 * std_b 用 TIM2 做行地址、TIM3 做行同步扫描、TIM7 做 HAL 时基。
 * **没有 TIM4** —— 与 std_a 不同，故 PL_TIM4 这项留空（枚举跨板稳定，
 * 共享代码按名字引用 PL_TIM4 的地方在 B 上拿到 NULL，不会编译不过）。
 *
 * 注意"哪个定时器干哪种活"与 std_a 也不同：B 的扫描是 TIM2、亮度 PWM 是 TIM3，
 * 见 boards/std_b/Inc/board.h 的 BOARD_DISPLAY_*_TIM。
 */

#include "pl_tim.h"
#include "tim.h"

/* TIM7 不在 tim.c 里：它是 HAL 时基，由 stm32f4xx_hal_timebase_tim.c 持有 */
extern TIM_HandleTypeDef htim7;

const pl_tim_board_entry_t g_pl_tim_board[PL_TIM_MAX] = {
    [PL_TIM2] = {.init = MX_TIM2_Init, .handle = &htim2, .irq = TIM2_IRQn},
    [PL_TIM3] = {.init = MX_TIM3_Init, .handle = &htim3, .irq = TIM3_IRQn},
    /* PL_TIM4 本板无此定时器，留空 */
    /* TIM7 无需 MX_ 初始化（HAL 时基自己配），故 .init 留空 */
    [PL_TIM7] = {.init = NULL, .handle = &htim7, .irq = TIM7_IRQn},
};
