/**
 * @file    dev_rs485.c
 * @brief   RS485 半双工收发器（USART1, RE=PA8）
 *
 * 板级实例化层：声明 UART 外设、DMA 缓冲区、ISR 适配回调，
 * 通道逻辑由 dev_uart_channel 统一提供。
 */

#include "dev_rs485.h"

#include "initcall.h"
#include "pl_uart.h"
#include "pl_gpio.h"
#include "dev_uart_channel.h"

#define RS485_BUF_SIZE (2048U)

/* ---- 静态资源 ---- */
static uint8_t s_rs485_buf[RS485_BUF_SIZE];

/* ---- 通道实例 ---- */
uart_channel_t g_rs485 = {.ch = {.ch_id = CH_ID_RS485}};

uart_channel_t *dev_rs485_get_channel(void)
{
    return &g_rs485;
}

/* ---- RS485 方向控制（板级引脚：PA8） ---- */
static void rs485_dir_cb(bool tx)
{
    pl_gpio_write(PL_PORT_A, 8, tx);
}

/* ---- 板级硬件初始化（hw_initcall，RTOS 前） ---- */
void dev_rs485_init(void)
{
    uart_channel_init(&g_rs485, pl_uart_get_handle(PL_UART1), CH_ID_RS485, true, s_rs485_buf, RS485_BUF_SIZE);
    pl_uart_set_dir_cb(pl_uart_get_handle(PL_UART1), rs485_dir_cb);
    rs485_dir_cb(false); /* 默认接收态 */
}
hw_dev_initcall(dev_rs485_init);
