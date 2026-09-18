/**
 * @file    pl_hub75.h
 * @brief   std_a 的 HUB75 接口 —— 引脚定义与高性能内联访问器
 *
 * **本头文件归板级**，不是共享 Platform 的一部分。原因：A/B 两块板的数据通道
 * 模型根本不同 —— std_a 是"R/G/B 各 10 路通道 + BSRR 预计算表"（g_hub75_pin_r/g/b），
 * B 板是"7 个端口的索引表、由面板驱动自持 channel_map"，连时钟脉冲的 NOP 数都不同
 * （本板 2，B 板 4）。硬抽一层共享接口只会把两边的硬件事实都拧弯。
 *
 * 共享代码（Device/Display/dev_display.c）只需要以下四样，换板时新板必须同样提供，
 * 否则编译不过 —— 契约由编译器保证，不需要额外的共享头：
 *     void pl_hub75_init(void);
 *     void pl_hub75_oe_set(bool blank);      // OE 低有效：true = 消隐
 *     void pl_hub75_latch_pulse(void);
 *     void pl_hub75_set_row(uint8_t row);
 * 面板驱动另外会用到本板自己的数据通道访问器（本文件下半部分）。
 *
 * 位带写法必须保留：访问器在扫描热路径上（每行 336 步 × 50 行），退化成函数调用
 * 会吃掉扫描预算。
 */

#pragma once

#include "main.h" /* CubeMX 引脚宏：HUB75_xx_Pin / HUB75_xx_GPIO_Port */
#include <stdint.h>
#include <stdbool.h>

/* ---- HUB75 接口形态 ---- */
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

/** @brief HUB75 引脚表（由 boards/std_a/Src/pl_hub75_board.c 定义） */
extern const hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX];
extern const hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX];
extern const hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX];

/* ---- 高性能内联：单通道 RGB 输出 ---- */
__STATIC_INLINE void pl_hub75_set_rgb(uint8_t ch, hub75_color_t color)
{
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
