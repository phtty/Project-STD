/**
 * @file    dev_rs485.h
 * @brief   RS485 半双工收发器（USART1, RE=PB12）
 *
 * 板级实例化层，通道逻辑由 dev_uart_channel 提供。
 */

#pragma once

#include "dev_uart_channel.h"

extern uart_channel_t g_rs485;

void dev_rs485_init(void);
uart_channel_t *dev_rs485_get_channel(void);
