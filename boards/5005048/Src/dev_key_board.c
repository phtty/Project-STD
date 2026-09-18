/**
 * @file    dev_key_board.c
 * @brief   5005048 的按键板级表
 *
 * 5005048 只有测试键 KEY_TST（PD8，EXTI 下降沿，内部上拉、低有效）。
 * 3833024 的 SW1~SW3 与 DIP1~DIP2 在本板不存在，故不在表里 ——
 * dev_key_get() 对这些 id 返回 NULL。
 */

#include "dev_key.h"
#include "main.h"

const dev_key_board_desc_t g_dev_key_board[] = {
    {.id = DEV_KEY_TST, .port = PL_PORT_D, .pin = 8, .active_low = true, .has_exti = true, .exti_pin = KEY_TST_Pin},
};

const uint32_t g_dev_key_board_count = sizeof(g_dev_key_board) / sizeof(g_dev_key_board[0]);
