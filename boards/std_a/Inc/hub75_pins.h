/**
 * @file    hub75_pins.h
 * @brief   std_a 的 HUB75 引脚宏（被 Platform/Inc/pl_hub75.h 包含）
 *
 * 与 board.h 分开是刻意的：board.h 只放"轻量、到处都能用"的板级常量（定时器角色等），
 * 而本文件要拉进 CubeMX 的 main.h（HAL 类型），不该让只想取个定时器 ID 的文件付这个代价。
 *
 * 位带写法必须保留：pl_hub75.h 的内联访问器在扫描热路径上（每行 336 步 × 50 行），
 * 退化成函数调用会吃掉扫描预算。
 */

#pragma once

#include "main.h" /* CubeMX 引脚宏：HUB75_xx_Pin / HUB75_xx_GPIO_Port */

/* ---- HUB75 接口形态 ---- */
#define HUB75_CHANNEL_MAX 10

#define HUB75_OE  BITBAND_PERIPH(&(HUB75_OE_GPIO_Port->ODR), 0)
#define HUB75_CLK BITBAND_PERIPH(&(HUB75_CLK_GPIO_Port->ODR), 1)
#define HUB75_LAT BITBAND_PERIPH(&(HUB75_LAT_GPIO_Port->ODR), 3)

#define HUB75_A BITBAND_PERIPH(&(HUB75_A_GPIO_Port->ODR), 4)
#define HUB75_B BITBAND_PERIPH(&(HUB75_B_GPIO_Port->ODR), 5)
#define HUB75_C BITBAND_PERIPH(&(HUB75_C_GPIO_Port->ODR), 6)
#define HUB75_D BITBAND_PERIPH(&(HUB75_D_GPIO_Port->ODR), 7)
