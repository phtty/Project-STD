/**
 * @file    pl_hub75.h
 * @brief   host 单测用的 HUB75 板级接口替身（遮蔽 boards/<板>/Platform/Inc/pl_hub75.h）
 *
 * 真头会拉进 `main.h`（CubeMX 引脚宏）与位带宏，host 编不了；但**形状是契约**：
 * 模组驱动（`dev_p10_32x16_2200001703.c` 之类）就是按这里的类型与函数名写的，
 * 所以影子必须**同形**——改真头而忘改这里，套件会编译失败（可接受的失败方式）。
 *
 * 与真头的差异只有两处，都是"被观测"的需要：
 *   1. `GPIO_TypeDef` 是本文件自定义的两字段结构体（真头是 CMSIS 的寄存器块），
 *      只保留 `BSRR`（`pl_hub75_bsrr_flush` 写的就是它）与 `ODR`；
 *   2. 四个控制访问器与 `pl_hub75_bsrr_flush` 改成**普通函数声明**，实现放在套件里
 *      —— 真头是 static inline 位带写，host 侧没法在事后观测，换成记录桩才能断言
 *      "这一拍到底把哪几根数据线推成了什么电平"。
 *
 * 引脚表 `g_hub75_pin_r/g/b` 由套件提供（用假的端口指针 + 一通道一位的引脚号），
 * 这样从 BSRR 值就能反解出"第几通道、R/G/B 哪一位"。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief 与真头一致的每色通道上限（本板 10 路） */
#define HUB75_CHANNEL_MAX 10

/** @brief host 侧的 GPIO 端口替身：只保留被写到的两个寄存器 */
typedef struct {
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
} GPIO_TypeDef;

/** @brief HUB75 单通道 RGB 引脚描述（同真头） */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} pl_hub75_pin_t;

/** @brief BSRR 预计算写入项（同真头：val 在前、port 在后） */
typedef struct {
    uint32_t      val;
    GPIO_TypeDef *port;
} pl_hub75_bsrr_t;

extern const pl_hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX];
extern const pl_hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX];
extern const pl_hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX];

/* 控制面：真头是 static inline 位带写，这里换成可观测的普通函数 */
void pl_hub75_bsrr_flush(const pl_hub75_bsrr_t *p);
void pl_hub75_clock_pulse(void);
void pl_hub75_latch_pulse(void);
void pl_hub75_oe_set(bool blank);
void pl_hub75_set_row(uint8_t row);
