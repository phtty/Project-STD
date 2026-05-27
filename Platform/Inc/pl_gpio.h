/**
 * @file    pl_gpio.h
 * @brief   GPIO 初始化 + 基本读写（Platform 层抽象，隔离芯片 GPIO 基地址）
 */

#pragma once

#include <stdint.h>

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
