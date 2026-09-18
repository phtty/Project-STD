/**
 * @file    pl_hub75.h
 * @brief   std_b 的 HUB75 接口 —— 引脚定义与高性能内联访问器
 *
 * **本头文件归板级**，不是共享 Platform 的一部分。原因：A/B 两块板的数据通道
 * 模型根本不同 —— std_a 是"R/G/B 各 10 路通道 + BSRR 预计算表"，本板是
 * "7 个端口的索引表、由面板驱动（dev_p10_112x10）自持 channel_map"，
 * 连时钟脉冲的 NOP 数都不同（本板 4，std_a 是 2）。
 *
 * 共享代码（Device/Display/dev_display.c）只需要以下四样，换板时新板必须同样提供，
 * 否则编译不过 —— 契约由编译器保证，不需要额外的共享头：
 *     void pl_hub75_init(void);
 *     void pl_hub75_oe_set(bool blank);      // OE 低有效：true = 消隐
 *     void pl_hub75_latch_pulse(void);
 *     void pl_hub75_set_row(uint8_t row);
 * 面板驱动另外会用到本板自己的数据通道访问器（pl_hub75_port_by_idx）。
 *
 * 位带写法必须保留：访问器在扫描热路径上，退化成函数调用会吃掉扫描预算。
 */

#pragma once

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- 控制信号 (bit-band, B 工程引脚) ---- */
#define HUB75_OE  BITBAND_PERIPH(&(LED_OE_GPIO_Port->ODR), 9)  /* PE9  */
#define HUB75_CLK BITBAND_PERIPH(&(LED_CLK_GPIO_Port->ODR), 8)  /* PE8  */
#define HUB75_LAT BITBAND_PERIPH(&(LED_LE_GPIO_Port->ODR), 7)   /* PE7  */

/* ---- 行地址选择 (bit-band, 标签交叉: A→LED_C, B→LED_D, C→LED_A, D→LED_B) ---- */
#define HUB75_A BITBAND_PERIPH(&(LED_C_GPIO_Port->ODR), 15)     /* PF15 */
#define HUB75_B BITBAND_PERIPH(&(LED_D_GPIO_Port->ODR), 14)     /* PF14 */
#define HUB75_C BITBAND_PERIPH(&(LED_A_GPIO_Port->ODR), 1)      /* PG1  */
#define HUB75_D BITBAND_PERIPH(&(LED_B_GPIO_Port->ODR), 0)      /* PG0  */

/* ---- 高性能内联：控制信号 ---- */

/* B 工程时序: 高低电平各 4 NOP */
__STATIC_INLINE void pl_hub75_clock_pulse(void)
{
    HUB75_CLK = 1;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    HUB75_CLK = 0;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
}

__STATIC_INLINE void pl_hub75_latch_pulse(void)
{
    HUB75_LAT = 1;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    HUB75_LAT = 0;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
}

/** @brief 消隐控制 (OE 低有效: true = 输出关闭/消隐) */
__STATIC_INLINE void pl_hub75_oe_set(bool blank)
{
    HUB75_OE = blank ? 1 : 0;
}

/** @brief 行地址编码 (1..2, 与 B 工程 scan_channel 一致) */
__STATIC_INLINE void pl_hub75_set_row(uint8_t row)
{
    HUB75_A = (row & 0x01) ? 1 : 0;
    HUB75_B = (row & 0x02) ? 1 : 0;
    HUB75_C = (row & 0x04) ? 1 : 0;
    HUB75_D = (row & 0x08) ? 1 : 0;
}

/* ---- 端口基址访问 (Device 层扫描用) ---- */

/** @brief 端口索引 0..6 → GPIOA..GPIOG (B 工程 channel_port 表) */
GPIO_TypeDef *pl_hub75_port_by_idx(uint8_t idx);

/* ---- 初始化 ---- */
void pl_hub75_init(void);
