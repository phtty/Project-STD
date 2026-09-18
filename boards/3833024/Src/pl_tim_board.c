/**
 * @file    pl_tim_board.c
 * @brief   3833024 的定时器板级表
 *
 * 共享的 Platform/Src/pl_tim.c 只认 g_pl_tim_board[]，本文件是它对这块板的答案：
 * 有哪些定时器、各自怎么初始化、句柄是谁、IRQ 号是多少。
 *
 * 3833024 用 TIM2 做行地址、TIM3 做行同步扫描、TIM4 做亮度 PWM、TIM7 做 HAL 时基。
 * 换板时改这张表，不要去改 pl_tim.c。
 */

#include "pl_tim.h"
#include "tim.h"

/* TIM7 不在 tim.c 里：它是 HAL 时基，由 stm32f4xx_hal_timebase_tim.c 持有 */
extern TIM_HandleTypeDef htim7;

const pl_tim_board_entry_t g_pl_tim_board[PL_TIM_MAX] = {
    [PL_TIM2] = {.init = MX_TIM2_Init, .handle = &htim2, .irq = TIM2_IRQn},
    [PL_TIM3] = {.init = MX_TIM3_Init, .handle = &htim3, .irq = TIM3_IRQn},
    [PL_TIM4] = {.init = MX_TIM4_Init, .handle = &htim4, .irq = TIM4_IRQn},
    /* TIM7 无需 MX_ 初始化（HAL 时基自己配），故 .init 留空 */
    [PL_TIM7] = {.init = NULL, .handle = &htim7, .irq = TIM7_IRQn},
};
