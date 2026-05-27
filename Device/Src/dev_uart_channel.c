/**
 * @file    dev_uart_channel.c
 * @brief   通用 UART 通道实现 — 收敛 RS485/RS232 共有的 send/task/init/deinit
 */

#include "dev_uart_channel.h"

/* ---- 统一 send：方向控制由 pl_uart_send 内部回调完成 ---- */
static int32_t uart_channel_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    uart_channel_t *self = container_of(ch, uart_channel_t, ch);
    return pl_uart_send(self->uart, data, len, 100);
}

const ch_ops_t uart_ch_ops = {.send = uart_channel_send};

/* ---- 通道生命周期（hw_initcall 阶段：仅设硬件参数，不调 RTOS API） ---- */
void uart_channel_init(uart_channel_t *self, pl_uart_handle_t uart,
                       uint8_t ch_id, bool rs485_mode,
                       uint8_t *rx_buf, uint16_t rx_buf_size)
{
    self->ch.ch_id    = ch_id;
    self->ch.ops      = &uart_ch_ops;
    self->ch.state    = CH_STATE_DOWN;
    self->uart        = uart;
    self->rs485_mode  = rs485_mode;
    self->rx_buf      = rx_buf;
    self->rx_buf_size = rx_buf_size;
}

void uart_channel_deinit(uart_channel_t *self)
{
    self->ch.state = CH_STATE_DOWN;
    app_channel_register(self->ch.ch_id, NULL);
}

/* ---- ISR 回调通知 + 注册 ---- */
static void uart_channel_isr_adapter(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    uart_channel_rx_indicate((uart_channel_t *)ctx, len);
}

void uart_channel_register_isr(uart_channel_t *self, pl_uart_handle_t uart)
{
    pl_uart_set_rx_cb(uart, uart_channel_isr_adapter, self);
}

/* ---- ISR 回调通知 ---- */
void uart_channel_rx_indicate(uart_channel_t *self, uint16_t len)
{
    osMessageQueuePut(self->rx_queue, &len, 0, 0);
}

/* ---- 统一任务入口（RTOS 启动后执行，创建队列 + 注册通道 + start_rx + 分发循环） ---- */
void uart_channel_task(void *argument)
{
    uart_channel_t *self = (uart_channel_t *)argument;

    self->rx_queue = osMessageQueueNew(1, sizeof(uint16_t), NULL);
    app_channel_register(self->ch.ch_id, &self->ch);
    self->ch.state = CH_STATE_UP;

    uart_channel_register_isr(self, self->uart);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);

    for (;;) {
        uint16_t rx_len = 0;
        osMessageQueueGet(self->rx_queue, &rx_len, 0, osWaitForever);
        app_channel_dispatch(&self->ch, self->rx_buf, rx_len);
    }
}
