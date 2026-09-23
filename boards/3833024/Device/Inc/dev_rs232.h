/**
 * @file    dev_rs232.h
 * @brief   RS232 串口板级资源（USART3=RS232-0, USART6=RS232-1）
 */
#pragma once

#include <stdint.h>

/** @brief 取 RS232 通道的 DMA 收发缓冲区
 *  @param index  通道号：0 = USART3(RS232-0)，1 = USART6(RS232-1)
 *  @return 缓冲区首地址（板级静态，长度 2048） */
uint8_t *dev_rs232_get_buf(uint8_t index);
