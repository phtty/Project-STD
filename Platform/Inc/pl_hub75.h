/**
 * @file    pl_hub75.h
 * @brief   HUB75 LED 点阵 GPIO 位拆裂抽象
 *
 * 封装所有 BITBAND_PERIPH 操作和 BSRR 直接寄存器写入，
 * 对外暴露高性能内联函数。
 *
 * 本文件是**共享接口**：内联访问器在热路径上（每行 336 步 × 50 行），
 * 不能退化成函数调用，所以引脚宏必须保持编译期可见 —— 由板级头 hub75_pins.h
 * 提供（HUB75_CHANNEL_MAX 与 HUB75_OE/CLK/LAT/A~D 的位带定义），
 * 引脚表 g_hub75_pin_r/g/b 与 pl_hub75_init 由板级源实现。
 */

#pragma once

#include "hub75_pins.h" /* 板级引脚宏：这些不是"共享"的，只是恰好同名 */
#include <stdint.h>

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

/** @brief 消隐控制。HUB75 的 OE 低有效：blank=true → OE 拉高 → 输出关断（屏黑）
 *
 *  形参此前叫 enable，与"true 表示消隐"正好相反，调用点读起来像"使能输出"
 *  实则相反，故更名 blank。 */
__STATIC_INLINE void pl_hub75_oe_set(bool blank)
{
    HUB75_OE = blank ? 1 : 0;
}

__STATIC_INLINE void pl_hub75_set_row(uint8_t row)
{
    HUB75_A = (row & 0x01) ? 1 : 0;
    HUB75_B = (row & 0x02) ? 1 : 0;
    HUB75_C = (row & 0x04) ? 1 : 0;
    HUB75_D = (row & 0x08) ? 1 : 0;
}

/* ---- BSRR 预计算值（Device 层可持有，通过 pl_hub75_bsrr_flush 写入，不碰 GPIO_TypeDef） ---- */
typedef struct {
    uint32_t      val;
    GPIO_TypeDef *port;
} pl_hub75_bsrr_t;

__STATIC_INLINE void pl_hub75_bsrr_flush(const pl_hub75_bsrr_t *p)
{
    p->port->BSRR = p->val;
}

/* ---- 初始化 ---- */
void pl_hub75_init(void);
