/**
 * @file    dev_rs485.c
 * @brief   RS485 半双工收发器板级资源（USART1, RE=PA8）
 *
 * 仅提供 DMA 缓冲区 + RE 方向控制等板级静态资源。
 * 通道生命周期和任务循环由 Application 层 app_rs485 负责。
 */

#include "dev_rs485.h"

#include "pl_uart.h"
#include "pl_gpio.h"
#include "initcall.h"

#define RS485_BUF_SIZE (2048U)

static uint8_t s_rs485_buf[RS485_BUF_SIZE];

uint8_t *dev_rs485_get_buf(void)
{
    return s_rs485_buf;
}

/* ---- RS485 方向控制（板级引脚：PA8） ---- */
static void rs485_dir_cb(bool tx)
{
    pl_gpio_write(PL_PORT_A, 8, tx);
}

void dev_rs485_init(void)
{
    pl_uart_set_dir_cb(pl_uart_get_handle(PL_UART1), rs485_dir_cb);
    rs485_dir_cb(false);
}
hw_dev_initcall(dev_rs485_init);
