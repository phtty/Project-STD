/**
 * @file    pl_hub75.h
 * @brief   HUB75 LED 点阵 GPIO 位拆裂抽象
 *
 * 封装所有 BITBAND_PERIPH 操作和 BSRR 直接寄存器写入，
 * 对外暴露高性能内联函数。换 MCU 只需重写此文件。
 */

#pragma once

#include "main.h"
#include <stdint.h>

/* ---- HUB75 通道数 ---- */
#define HUB75_CHANNEL_MAX 10

/* ---- 控制信号 (bit-band) ---- */
#define HUB75_OE  BITBAND_PERIPH(&(HUB75_OE_GPIO_Port->ODR), 0)
#define HUB75_CLK BITBAND_PERIPH(&(HUB75_CLK_GPIO_Port->ODR), 1)
#define HUB75_LAT BITBAND_PERIPH(&(HUB75_LAT_GPIO_Port->ODR), 3)

/* ---- 行地址选择 (bit-band) ---- */
#define HUB75_A BITBAND_PERIPH(&(HUB75_A_GPIO_Port->ODR), 4)
#define HUB75_B BITBAND_PERIPH(&(HUB75_B_GPIO_Port->ODR), 5)
#define HUB75_C BITBAND_PERIPH(&(HUB75_C_GPIO_Port->ODR), 6)
#define HUB75_D BITBAND_PERIPH(&(HUB75_D_GPIO_Port->ODR), 7)

/** @brief HUB75 单通道 RGB 引脚描述 */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} hub75_pin_t;

/** @brief HUB75 颜色枚举 (bit0=R, bit1=G, bit2=B) */
typedef enum {
    HUB75_COLOR_BLACK  = 0,
    HUB75_COLOR_RED    = 1,
    HUB75_COLOR_GREEN  = 2,
    HUB75_COLOR_YELLOW = 3,
    HUB75_COLOR_BLUE   = 4,
    HUB75_COLOR_PURPLE = 5,
    HUB75_COLOR_CYAN   = 6,
    HUB75_COLOR_WHITE  = 7,
} hub75_color_t;

/** @brief HUB75 引脚表（由 dev_display 初始化时设定） */
extern const hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX];
extern const hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX];
extern const hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX];

/* ---- 高性能内联：单通道 RGB 输出 ---- */

__STATIC_INLINE void pl_hub75_set_rgb(uint8_t ch, hub75_color_t color)
{
    if (ch >= HUB75_CHANNEL_MAX) return;
    GPIO_TypeDef *rp = g_hub75_pin_r[ch].port;
    GPIO_TypeDef *gp = g_hub75_pin_g[ch].port;
    GPIO_TypeDef *bp = g_hub75_pin_b[ch].port;
    uint16_t rm      = g_hub75_pin_r[ch].pin;
    uint16_t gm      = g_hub75_pin_g[ch].pin;
    uint16_t bm      = g_hub75_pin_b[ch].pin;

    if (color & 1) {
        rp->BSRR = rm;
    } else {
        rp->BSRR = rm << 0x10;
    }
    if (color & 2) {
        gp->BSRR = gm;
    } else {
        gp->BSRR = gm << 0x10;
    }
    if (color & 4) {
        bp->BSRR = bm;
    } else {
        bp->BSRR = bm << 0x10;
    }
}

__STATIC_INLINE void pl_hub75_clock_pulse(void)
{
    HUB75_CLK = 1;
    __NOP();
    __NOP();
    HUB75_CLK = 0;
}

__STATIC_INLINE void pl_hub75_latch_pulse(void)
{
    HUB75_LAT = 1;
    __NOP();
    __NOP();
    HUB75_LAT = 0;
}

__STATIC_INLINE void pl_hub75_oe_set(bool enable)
{
    HUB75_OE = enable ? 1 : 0;
}

__STATIC_INLINE void pl_hub75_set_row(uint8_t row)
{
    HUB75_A = (row & 0x01) ? 1 : 0;
    HUB75_B = (row & 0x02) ? 1 : 0;
    HUB75_C = (row & 0x04) ? 1 : 0;
    HUB75_D = (row & 0x08) ? 1 : 0;
}

/* ---- 初始化 ---- */
void pl_hub75_init(void);
