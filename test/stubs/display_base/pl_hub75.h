/**
 * @file    pl_hub75.h
 * @brief   host 单测用的 `pl_hub75` 替身 —— 只服务 `test_display_base`
 *
 * 真头（`boards/<板>/Platform/Inc/pl_hub75.h`）是位带直写 ODR，host 上没有
 * 位带寄存器。`dev_display.c` 只需要这四个契约（该头注释里写明的共享面）：
 * 换板时新板必须同样提供，否则共享代码编不过 —— 这里按同一份契约声明。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void pl_hub75_init(void);
void pl_hub75_oe_set(bool blank); /**< OE 低有效：true = 消隐 */
void pl_hub75_latch_pulse(void);
void pl_hub75_set_row(uint8_t row);
