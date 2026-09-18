/**
 * @file    pl_tim_board.c
 * @brief   3833024 的定时器板级表
 *
 * 共享的 Platform/Src/pl_tim.c 只认 g_pl_tim_board[]，本文件是它对这块板的答案：
 * 有哪些定时器、各自怎么初始化、句柄是谁、IRQ 号是多少。
 *
 * **角色映射不在这里，在 boards/3833024/Inc/board.h 的 BOARD_DISPLAY_*_TIM**
 * （扫描 = TIM3，亮度 PWM = TIM4）。本表只回答"有哪些定时器、怎么初始化、句柄是谁"，
 * 不回答"谁干什么" —— 两块板的角色是错位的（5006048 是扫描 TIM2 / PWM TIM3），
 * 光看这张表会误判。
 *
 * TIM2 本板配了但**没有任何消费者**（不 start_it、不注册回调）—— 疑似历史遗留。
 * 行地址不是定时器做的，是 pl_hub75_set_row() 直接用 GPIO。
 *
 * 换板时改这张表与 board.h，不要去改 pl_tim.c。
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
