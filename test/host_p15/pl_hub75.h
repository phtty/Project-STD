/**
 * @file    pl_hub75.h
 * @brief   host 单测用的 5006048 pl_hub75.h 替身 —— 只够编/驱动 P15 64×8
 *
 * 真头是**位带直接写 ODR**（见 boards/5006048/Platform/Inc/pl_hub75.h）；host
 * 上没有位带，这里换成 (端口 × 位) 的影子表 —— 于是"控制线电平"能被测试读回，
 * `_set_row()` 是否把 A~D 拉低可以断言。
 *
 * 只保留 P15 驱动实际依赖的那部分契约：`HUB75_A..D`、`pl_hub75_clock_pulse()`、
 * `pl_hub75_port_by_idx()`；其余（OE/LAT/init/set_row）给同样形状的替身，保证
 * 真头的公共面改了名时本套件会编译失败，而不是静默走另一套。
 */

#pragma once

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief 控制线影子位表：[端口 0..6][位 0..15]（定义在 test_p15_mapping.c） */
extern uint8_t g_p15_host_bit[7][16];

#define HUB75_OE  (g_p15_host_bit[4][9])  /**< PE9，低有效 */
#define HUB75_CLK (g_p15_host_bit[4][8])  /**< PE8 */
#define HUB75_LAT (g_p15_host_bit[4][7])  /**< PE7 */
#define HUB75_A   (g_p15_host_bit[5][15]) /**< PF15（标签是 LED_C） */
#define HUB75_B   (g_p15_host_bit[5][14]) /**< PF14（标签是 LED_D） */
#define HUB75_C   (g_p15_host_bit[6][1])  /**< PG1（标签是 LED_A） */
#define HUB75_D   (g_p15_host_bit[6][0])  /**< PG0（标签是 LED_B） */

/** @brief 移位时钟脉冲计数（测试用它断言一帧正好 384 步） */
extern uint16_t g_p15_host_clk_pulses;

/** @brief 发一个移位时钟脉冲（host：只记次数） */
static inline void pl_hub75_clock_pulse(void)
{
    g_p15_host_clk_pulses++;
}

/** @brief 发一个锁存脉冲（host：无副作用） */
static inline void pl_hub75_latch_pulse(void)
{
}

/** @brief 消隐控制（host：只写影子位） */
static inline void pl_hub75_oe_set(bool blank)
{
    HUB75_OE = blank ? 1 : 0;
}

/** @brief 行地址编码（host：自然二进制位序，与真头一致） */
static inline void pl_hub75_set_row(uint8_t row)
{
    HUB75_A = (row & 0x01U) ? 1 : 0;
    HUB75_B = (row & 0x02U) ? 1 : 0;
    HUB75_C = (row & 0x04U) ? 1 : 0;
    HUB75_D = (row & 0x08U) ? 1 : 0;
}

/** @brief 端口索引 0..6 → GPIOA..GPIOG（定义在 test_p15_mapping.c） */
GPIO_TypeDef *pl_hub75_port_by_idx(uint8_t idx);

/** @brief 初始化（host：无副作用，只为保持真头的公共面） */
void pl_hub75_init(void);
