/**
 * @file    app_rs485.c
 * @brief   RS485 半双工通道（USART1, RE 方向由 dev_rs485 注入）
 *
 * 板级资源（DMA 缓冲区、RE 方向控制）由 Device 层 dev_rs485 提供，
 * 通道生命周期和收发任务循环全部在 Application 层实现。
 */

#include "app_rs485.h"

#include "pl_uart.h"
#include "dev_rs485.h"
#include "app_dispatch.h"

typedef struct {
    channel_t me;
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
} rs485_ch_t;

#define RS485_BUF_SIZE (2048U)

/* ---- 实例 ---- */
static rs485_ch_t g_rs485 = {.me = {.ch_id = CH_ID_RS485}};

/* ---- ops ---- */
static int32_t rs485_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    rs485_ch_t *self = container_of(ch, rs485_ch_t, me);
    return pl_uart_send(self->uart, data, len, 100);
}

static const ch_ops_t rs485_ops = {.send = rs485_send};

/* ---- ISR → 任务通知 ---- */
static void rs485_isr_cb(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    rs485_ch_t *self = (rs485_ch_t *)ctx;
    osMessageQueuePut(self->rx_queue, &len, 0, 0);
}

/* ---- 任务循环 ---- */
static void rs485_task(void *argument)
{
    rs485_ch_t *self = (rs485_ch_t *)argument;

    self->rx_queue = osMessageQueueNew(1, sizeof(uint16_t), NULL);
    if (self->rx_queue == NULL) {
        osThreadExit();
    }
    app_channel_register(self->me.ch_id, &self->me);

    pl_uart_set_rx_cb(self->uart, rs485_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);

    for (;;) {
        uint16_t rx_len = 0;
        if (osMessageQueueGet(self->rx_queue, &rx_len, 0, osWaitForever) == osOK) {
            app_channel_dispatch(&self->me, self->rx_buf, rx_len);
        }
    }
}

/* ---- 公开 API ---- */

osThreadId_t app_rs485_start(void)
{
    rs485_ch_t *self  = &g_rs485;
    self->me.ops      = &rs485_ops;
    self->uart        = pl_uart_get_handle(PL_UART1);
    self->rx_buf      = dev_rs485_get_buf();
    self->rx_buf_size = RS485_BUF_SIZE;

    osThreadAttr_t rs485_task_attr = {
        .name       = "rs485_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    return osThreadNew(rs485_task, self, &rs485_task_attr);
}
