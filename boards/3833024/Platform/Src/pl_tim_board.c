/**
 * @file    pl_tim_board.c
 * @brief   3833024 的定时器板级表
 *
 * 共享的 Platform/Src/pl_tim.c 只认 g_pl_tim_board[]，本文件是它对这块板的答案：
 * 有哪些定时器、各自怎么初始化、句柄是谁、IRQ 号是多少。
 *
 * 3833024 配 TIM3（扫描节拍）与 TIM4（OE 亮度 PWM），两者角色由 Platform/Inc/pl_tim.h
 * 的 PL_TIM_DISPLAY_* 固定 —— 那是 MCU 级常量，两块板一致，不在板级表里表达。
 * 本表只回答"有哪些定时器、怎么初始化、句柄是谁"。
 *
 * TIM2 已移除：它曾被配成 2Hz（= 500ms 的"半秒定时器"），半秒业务改用 osDelay(500)
 * 走 RTOS tick 之后就没有消费者了，与 5006048 对齐时一并删掉。
 */

#include "pl_tim.h"
#include "tim.h"

/* TIM7 不在 tim.c 里：它是 HAL 时基，由 stm32f4xx_hal_timebase_tim.c 持有 */
extern TIM_HandleTypeDef htim7;

const pl_tim_board_entry_t g_pl_tim_board[PL_TIM_MAX] = {
    [PL_TIM3] = {.init = MX_TIM3_Init, .handle = &htim3, .irq = TIM3_IRQn},
    [PL_TIM4] = {.init = MX_TIM4_Init, .handle = &htim4, .irq = TIM4_IRQn},
    /* TIM7 无需 MX_ 初始化（HAL 时基自己配），故 .init 留空 */
    [PL_TIM7] = {.init = NULL, .handle = &htim7, .irq = TIM7_IRQn},
};
