/**
 * @file    dev_key_board.c
 * @brief   3833024 的按键板级表
 *
 * 3833024 六个输入：
 *   PE12/PE11/PE10  SW1~SW3   干接点，EXTI 下降沿
 *   PD8             KEY_TST   测试键，EXTI 下降沿
 *   PE8/PE9         DIP1~DIP2 拨码开关，纯轮询（无 EXTI）
 *
 * **DIP 的引脚以原理图为准 = PE8(DIP1)/PE9(DIP2)**。旧版 CubeMX 工程把它们
 * 错标成 PE7/PE8，于是 bit0 永远读到 PE7（悬空上拉恒为 1）—— 地址只可能是 0/2，
 * 从卡永远编不成 addr=1。现象是"拨了 DIP1，地址却变成 2、屏不亮"。
 *
 * 全部内部上拉、低有效。
 */

#include "dev_key.h"
#include "main.h"

const dev_key_board_desc_t g_dev_key_board[] = {
    {.id = DEV_KEY_SW1,  .port = PL_PORT_E, .pin = 12, .active_low = true, .has_exti = true,  .exti_pin = SW1_Pin},
    {.id = DEV_KEY_SW2,  .port = PL_PORT_E, .pin = 11, .active_low = true, .has_exti = true,  .exti_pin = SW2_Pin},
    {.id = DEV_KEY_SW3,  .port = PL_PORT_E, .pin = 10, .active_low = true, .has_exti = true,  .exti_pin = SW3_Pin},
    {.id = DEV_KEY_TST,  .port = PL_PORT_D, .pin =  8, .active_low = true, .has_exti = true,  .exti_pin = KEY_TST_Pin},
    {.id = DEV_KEY_DIP1, .port = PL_PORT_E, .pin =  8, .active_low = true, .has_exti = false, .exti_pin = 0},
    {.id = DEV_KEY_DIP2, .port = PL_PORT_E, .pin =  9, .active_low = true, .has_exti = false, .exti_pin = 0},
};

const uint32_t g_dev_key_board_count = sizeof(g_dev_key_board) / sizeof(g_dev_key_board[0]);
