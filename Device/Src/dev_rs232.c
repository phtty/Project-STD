/**
 * @file    dev_rs232.c
 * @brief   RS232 串口板级实例化（USART3=RS232-0, USART6=RS232-1，全双工）
 *
 * 板级实例化层：声明 UART 外设、DMA 缓冲区、ISR 适配回调，
 * 通道逻辑由 dev_uart_channel 统一提供。
 */

#include "dev_rs232.h"
#include "dev_uart_channel.h"
#include "pl_uart.h"
#include "initcall.h"

#define RS232_BUF_SIZE (2048U)

/* ---- 静态资源 ---- */
static uint8_t s_rs232_0_buf[RS232_BUF_SIZE];
static uint8_t s_rs232_1_buf[RS232_BUF_SIZE];

/* ---- 通道实例 ---- */
uart_channel_t g_rs232_0 = {.ch = {.ch_id = CH_ID_RS232}};
uart_channel_t g_rs232_1 = {.ch = {.ch_id = CH_ID_RS232_1}};

uart_channel_t *dev_rs232_get_channel(uint8_t index)
{
    return (index == 0) ? &g_rs232_0 : &g_rs232_1;
}

/* ---- 板级硬件初始化（hw_initcall，RTOS 前） ---- */
void dev_rs232_init(void)
{
    pl_uart_init();
    uart_channel_init(&g_rs232_0, pl_uart_get_handle(PL_UART3), CH_ID_RS232,    false, s_rs232_0_buf, RS232_BUF_SIZE);
    uart_channel_init(&g_rs232_1, pl_uart_get_handle(PL_UART6), CH_ID_RS232_1, false, s_rs232_1_buf, RS232_BUF_SIZE);
}
hw_driver_initcall(dev_rs232_init);
