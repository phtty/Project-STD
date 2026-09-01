/**
 * @file    dev_rs232.h
 * @brief   RS232 串口板级资源（USART3=RS232-0 协议 RX，USART6=语音 TX 无 DMA RX）
 */
#pragma once

#include <stdint.h>

/* 乒乓双缓冲单块大小（仅 index0）：≥ QH 最大帧 259，且 ≤ RB_SIZE_RS232−1。
 * 实际 DMA 缓冲 = 2 × RS232_BUF_SIZE = 1280B（pl_uart circular 周期）。 */
#define RS232_BUF_SIZE (640U)

uint8_t *dev_rs232_get_buf(uint8_t index);