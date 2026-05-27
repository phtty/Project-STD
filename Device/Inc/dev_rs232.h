/**
 * @file    dev_rs232.h
 * @brief   RS232 串口（USART3=RS232-0, USART6=RS232-1，全双工）
 *
 * 板级实例化层，通道逻辑由 dev_uart_channel 提供。
 */

#pragma once

#include "dev_uart_channel.h"

extern uart_channel_t g_rs232_0;
extern uart_channel_t g_rs232_1;

void dev_rs232_init(void);
uart_channel_t *dev_rs232_get_channel(uint8_t index);
