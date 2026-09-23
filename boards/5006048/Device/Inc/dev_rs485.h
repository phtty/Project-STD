/**
 * @file    dev_rs485.h
 * @brief   RS485 半双工收发器板级资源（USART1, RE=PA8）
 */
#pragma once

#include <stdint.h>

void dev_rs485_init(void);
uint8_t *dev_rs485_get_buf(void);
