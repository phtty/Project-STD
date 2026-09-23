/**
 * @file    pl_hub75.h
 * @brief   3833024 的 HUB75 接口 —— 引脚定义与高性能内联访问器
 *
 * **本头文件归板级**，不是共享 Platform 的一部分。原因：A/B 两块板的数据通道
 * 模型根本不同 —— 3833024 是"R/G/B 各 10 路通道 + BSRR 预计算表"（g_hub75_pin_r/g/b），
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
#define HUB75_CHANNEL_MAX 10    /**< 每色（R/G/B）的数据通道数 */

/* ---- 控制信号 (bit-band) ---- */
#define HUB75_OE  BITBAND_PERIPH(&(HUB75_OE_GPIO_Port->ODR), 0)  /**< 输出使能位带（低有效） */
#define HUB75_CLK BITBAND_PERIPH(&(HUB75_CLK_GPIO_Port->ODR), 1) /**< 移位时钟位带 */
#define HUB75_LAT BITBAND_PERIPH(&(HUB75_LAT_GPIO_Port->ODR), 3) /**< 锁存脉冲位带 */

/* ---- 行地址选择 (bit-band) ---- */
#define HUB75_A BITBAND_PERIPH(&(HUB75_A_GPIO_Port->ODR), 4) /**< 行地址 bit0 位带 */
#define HUB75_B BITBAND_PERIPH(&(HUB75_B_GPIO_Port->ODR), 5) /**< 行地址 bit1 位带 */
#define HUB75_C BITBAND_PERIPH(&(HUB75_C_GPIO_Port->ODR), 6) /**< 行地址 bit2 位带 */
#define HUB75_D BITBAND_PERIPH(&(HUB75_D_GPIO_Port->ODR), 7) /**< 行地址 bit3 位带 */

/** @brief HUB75 单通道 RGB 引脚描述 */
typedef struct {
    GPIO_TypeDef *port;    /**< GPIO 端口 */
    uint16_t pin;          /**< 引脚号 */
} pl_hub75_pin_t;

/** @brief HUB75 引脚表（由 boards/3833024/Platform/Src/pl_hub75_board.c 定义） */
extern const pl_hub75_pin_t g_hub75_pin_r[HUB75_CHANNEL_MAX];
extern const pl_hub75_pin_t g_hub75_pin_g[HUB75_CHANNEL_MAX];    /**< G 通道引脚表 */
extern const pl_hub75_pin_t g_hub75_pin_b[HUB75_CHANNEL_MAX];    /**< B 通道引脚表 */

/** @brief 发一个移位时钟脉冲（含满足时序的 NOP） */
__STATIC_INLINE void pl_hub75_clock_pulse(void)
{
    HUB75_CLK = 1;
    __NOP();
    __NOP();
    HUB75_CLK = 0;
}

/** @brief 发一个锁存脉冲 */
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

/** @brief 按二进制位序驱动行地址 A~D
 *  @param row 行号（bit0→A，bit1→B，bit2→C，bit3→D） */
__STATIC_INLINE void pl_hub75_set_row(uint8_t row)
{
    HUB75_A = (row & 0x01) ? 1 : 0;
    HUB75_B = (row & 0x02) ? 1 : 0;
    HUB75_C = (row & 0x04) ? 1 : 0;
    HUB75_D = (row & 0x08) ? 1 : 0;
}

/* ---- BSRR 预计算值（Device 层可持有，通过 pl_hub75_bsrr_flush 写入，不碰 GPIO_TypeDef） ---- */
/** @brief BSRR 预计算写入项：一次写端口即可置位/复位多个通道 */
typedef struct {
    uint32_t      val;     /**< 预计算的 BSRR 值 */
    GPIO_TypeDef *port;    /**< 目标 GPIO 端口 */
} pl_hub75_bsrr_t;

/** @brief 把预计算的 BSRR 值写入端口（扫描热路径，无分支）
 *  @param p BSRR 预计算项（只读） */
__STATIC_INLINE void pl_hub75_bsrr_flush(const pl_hub75_bsrr_t *p)
{
    p->port->BSRR = p->val;
}

/* ---- 初始化 ---- */
/** @brief 初始化 HUB75 控制与数据引脚（hw_dev_initcall 阶段） */
void pl_hub75_init(void);
