/**
 * @file    dev_rs485.h
 * @brief   RS485 半双工收发器板级资源（USART1, RE=PA8）
 */
#pragma once

#include <stdint.h>

/** @brief 初始化 RS485 收发器（注册 RE 方向回调，USART1） */
void dev_rs485_init(void);

/** @brief 取 RS485 的 DMA 收发缓冲区
 *  @return 缓冲区首地址（板级静态，长度 2048） */
uint8_t *dev_rs485_get_buf(void);
