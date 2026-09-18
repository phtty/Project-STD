/**
 * @file    dev_rs232.h
 * @brief   RS232 串口板级资源（USART3=RS232-0, USART6=RS232-1）
 */
#pragma once

#include <stdint.h>

uint8_t *dev_rs232_get_buf(uint8_t index);
