/**
 * @file    dev_rs232.c
 * @brief   RS232 串口板级资源（USART3=RS232-0, USART6=RS232-1）
 *
 * 仅提供 DMA 缓冲区等板级静态资源。
 * 通道生命周期和任务循环由 Application 层 app_rs232 负责。
 */

#include "dev_rs232.h"

#define RS232_BUF_SIZE (2048U)

static uint8_t s_rs232_0_buf[RS232_BUF_SIZE];
static uint8_t s_rs232_1_buf[RS232_BUF_SIZE];

uint8_t *dev_rs232_get_buf(uint8_t index)
{
    return (index == 0) ? s_rs232_0_buf : s_rs232_1_buf;
}
