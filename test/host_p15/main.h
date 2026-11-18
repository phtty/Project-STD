/**
 * @file    main.h
 * @brief   host 单测用的 5006048 main.h 替身 —— 只够编 P15 64×8 驱动
 *
 * 与 `test/stubs/main.h` 的分工：那个是"只用到基础类型"的最小影子；本文件
 * 额外带上 **P15 驱动真正引用的那 32 根数据线**与 GPIO 实例形状。
 *
 * 引脚位号/端口与 `boards/5006048/Core/Inc/main.h` 逐条一致 —— 这是硬件事实，
 * 照抄它，"哪根线"在测试里才有意义。端口实例是假的：host 上不关心寄存器地址，
 * 只关心 BSRR 被写成了什么。
 *
 * 失败方式：真 main.h 增删/改名了驱动用到的引脚 → 本文件编译失败（而不是
 * 静默漂移成另一套引脚表）。
 */

#pragma once

#include <stdint.h>

/** @brief GPIO 寄存器形状的 host 替身（驱动只写 BSRR；ODR 给控制信号影子用） */
typedef struct {
    volatile uint32_t BSRR; /**< 置位/复位寄存器（扫描热路径唯一写的） */
    volatile uint32_t ODR;  /**< 输出数据寄存器（控制信号替身用） */
} GPIO_TypeDef;

/** @brief 7 个端口实例（定义在 test_p15_mapping.c） */
extern GPIO_TypeDef g_p15_host_gpio[7];

#define GPIOA (&g_p15_host_gpio[0]) /**< 假 GPIOA */
#define GPIOB (&g_p15_host_gpio[1]) /**< 假 GPIOB */
#define GPIOC (&g_p15_host_gpio[2]) /**< 假 GPIOC */
#define GPIOD (&g_p15_host_gpio[3]) /**< 假 GPIOD */
#define GPIOE (&g_p15_host_gpio[4]) /**< 假 GPIOE */
#define GPIOF (&g_p15_host_gpio[5]) /**< 假 GPIOF */
#define GPIOG (&g_p15_host_gpio[6]) /**< 假 GPIOG */

#define GPIO_PIN_0  (1U << 0)  /**< 引脚 0 */
#define GPIO_PIN_1  (1U << 1)  /**< 引脚 1 */
#define GPIO_PIN_2  (1U << 2)  /**< 引脚 2 */
#define GPIO_PIN_3  (1U << 3)  /**< 引脚 3 */
#define GPIO_PIN_4  (1U << 4)  /**< 引脚 4 */
#define GPIO_PIN_5  (1U << 5)  /**< 引脚 5 */
#define GPIO_PIN_6  (1U << 6)  /**< 引脚 6 */
#define GPIO_PIN_7  (1U << 7)  /**< 引脚 7 */
#define GPIO_PIN_8  (1U << 8)  /**< 引脚 8 */
#define GPIO_PIN_9  (1U << 9)  /**< 引脚 9 */
#define GPIO_PIN_10 (1U << 10) /**< 引脚 10 */
#define GPIO_PIN_11 (1U << 11) /**< 引脚 11 */
#define GPIO_PIN_12 (1U << 12) /**< 引脚 12 */
#define GPIO_PIN_13 (1U << 13) /**< 引脚 13 */
#define GPIO_PIN_14 (1U << 14) /**< 引脚 14 */
#define GPIO_PIN_15 (1U << 15) /**< 引脚 15 */

/* ---- P15 驱动用到的 32 根数据线（位号照 5006048 main.h） ----
 * 只列驱动实际引用的那些；每行末注释是"端口.位"。 */

#define LED_CH0_Pin  GPIO_PIN_6  /**< PA6 */
#define LED_CH1_Pin  GPIO_PIN_4  /**< PA4 */
#define LED_CH2_Pin  GPIO_PIN_3  /**< PA3 */
#define LED_CH3_Pin  GPIO_PIN_3  /**< PC3 */

#define LED_CH6_Pin  GPIO_PIN_8  /**< PF8 */
#define LED_CH7_Pin  GPIO_PIN_7  /**< PF7 */
#define LED_CH8_Pin  GPIO_PIN_6  /**< PF6 */
#define LED_CH9_Pin  GPIO_PIN_5  /**< PF5 */

#define LED_CH12_Pin GPIO_PIN_2  /**< PF2 */
#define LED_CH13_Pin GPIO_PIN_1  /**< PF1 */
#define LED_CH14_Pin GPIO_PIN_0  /**< PF0 */
#define LED_CH15_Pin GPIO_PIN_15 /**< PC15 */

#define LED_CH18_Pin GPIO_PIN_6  /**< PE6 */
#define LED_CH19_Pin GPIO_PIN_5  /**< PE5 */
#define LED_CH20_Pin GPIO_PIN_4  /**< PE4 */
#define LED_CH21_Pin GPIO_PIN_3  /**< PE3 */

#define LED_CH24_Pin GPIO_PIN_7  /**< PD7 */
#define LED_CH25_Pin GPIO_PIN_6  /**< PD6 */
#define LED_CH26_Pin GPIO_PIN_5  /**< PD5 */
#define LED_CH27_Pin GPIO_PIN_4  /**< PD4 */

#define LED_CH30_Pin GPIO_PIN_1  /**< PD1 */
#define LED_CH31_Pin GPIO_PIN_0  /**< PD0 */
#define LED_CH32_Pin GPIO_PIN_12 /**< PC12 */
#define LED_CH33_Pin GPIO_PIN_12 /**< PA12 */

#define LED_CH36_Pin GPIO_PIN_8  /**< PC8 */
#define LED_CH37_Pin GPIO_PIN_8  /**< PG8 */
#define LED_CH38_Pin GPIO_PIN_7  /**< PG7 */
#define LED_CH39_Pin GPIO_PIN_6  /**< PG6 */

#define LED_CH42_Pin GPIO_PIN_2  /**< PG2 */
#define LED_CH43_Pin GPIO_PIN_15 /**< PD15 */
#define LED_CH44_Pin GPIO_PIN_14 /**< PD14 */
#define LED_CH45_Pin GPIO_PIN_13 /**< PD13 */
