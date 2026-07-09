/**
 * @file    app_rs232.c
 * @brief   RS232 通道（USART3=RS232-0, USART6=RS232-1）
 *
 * 板级资源（DMA 缓冲区）由 Device 层 dev_rs232 提供，
 * 通道生命周期和收发任务循环全部在 Application 层实现。
 */

#include "app_rs232.h"

#include "pl_uart.h"
#include "dev_rs232.h"
#include "app_dispatch.h"

typedef struct {
    channel_t me;
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
} rs232_ch_t;

#define RS232_BUF_SIZE (2048U)

/* ---- 实例 ---- */
static rs232_ch_t g_rs232_0 = {.me = {.ch_id = CH_ID_RS232}};
static rs232_ch_t g_rs232_1 = {.me = {.ch_id = CH_ID_RS232_1}};

/* ---- ops ---- */
static int32_t rs232_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    rs232_ch_t *self = container_of(ch, rs232_ch_t, me);
    return pl_uart_send(self->uart, data, len, 100);
}

static const ch_ops_t rs232_ops = {.send = rs232_send};

/* ---- ISR → 任务通知 ---- */
static void rs232_isr_cb(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    rs232_ch_t *self = (rs232_ch_t *)ctx;
    osMessageQueuePut(self->rx_queue, &len, 0, 0);
}

/* ---- 任务循环 ---- */
static void rs232_task(void *argument)
{
    rs232_ch_t *self = (rs232_ch_t *)argument;

    self->rx_queue = osMessageQueueNew(1, sizeof(uint16_t), NULL);
    app_channel_register(self->me.ch_id, &self->me);

    pl_uart_set_rx_cb(self->uart, rs232_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);

    for (;;) {
        uint16_t rx_len = 0;
        osMessageQueueGet(self->rx_queue, &rx_len, 0, osWaitForever);
        app_channel_dispatch(&self->me, self->rx_buf, rx_len);
    }
}

/* ---- 公开 API ---- */

osThreadId_t app_rs232_start(void)
{
    rs232_ch_t *self = &g_rs232_0;
    self->me.ops      = &rs232_ops;
    self->uart        = pl_uart_get_handle(PL_UART3);
    self->rx_buf      = dev_rs232_get_buf(0);
    self->rx_buf_size = RS232_BUF_SIZE;
    return osThreadNew(rs232_task, self, &rs232_0_task_attr);
}

osThreadId_t app_rs232_1_start(void)
{
    rs232_ch_t *self = &g_rs232_1;
    self->me.ops      = &rs232_ops;
    self->uart        = pl_uart_get_handle(PL_UART6);
    self->rx_buf      = dev_rs232_get_buf(1);
    self->rx_buf_size = RS232_BUF_SIZE;
    return osThreadNew(rs232_task, self, &rs232_1_task_attr);
}
