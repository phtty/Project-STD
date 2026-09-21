/**
 * @file    pl_gpio.h
 * @brief   GPIO 初始化 + 基本读写（Platform 层抽象，隔离芯片 GPIO 基地址）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h> /* pl_gpio_write/read 用 bool —— 之前漏了，靠包含方先带进来才编得过 */

/** @brief 端口标识（不与芯片 GPIO 基地址耦合） */
typedef enum {
    PL_PORT_A = 0,
    PL_PORT_B,
    PL_PORT_C,
    PL_PORT_D,
    PL_PORT_E,
    PL_PORT_F,
    PL_PORT_G,
    PL_PORT_H,
    PL_PORT_MAX,
} pl_port_t;

void pl_gpio_init(void);
void pl_gpio_write(pl_port_t port, uint8_t pin, bool high);
bool pl_gpio_read(pl_port_t port, uint8_t pin);

/** @brief 使能某端口的外设时钟
 *
 *  **芯片级事实**：F4 每个 GPIO 各占 RCC 的一位，没有规律可循，只能逐个列。
 *  收在这里而不是让各模块自己写 if 链 —— "端口 → 硬件"的映射本模块已经有一份
 *  （g_port_base[]），再抄一份迟早会漂移。 */
void pl_gpio_clk_enable(pl_port_t port);

/** @brief 取端口寄存器基址，供需要直接配寄存器的 Platform 模块用（如 ETH 的复用脚配置）
 *
 *  返回 void* 而非 GPIO_TypeDef*：本头是共享 Platform 头，**不外泄 HAL 类型**
 *  （与 pl_tim_board_entry_t 用 void* 存句柄同一约定）。调用方同为 Platform 层、
 *  本来就在 include HAL，自行转回。端口非法时返回 NULL。 */
void *pl_gpio_port_base(pl_port_t port);
