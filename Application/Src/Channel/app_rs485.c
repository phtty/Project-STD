/**
 * @file    app_rs485.c
 * @brief   RS485 半双工通道（USART1, RE 方向由 dev_rs485 注入）
 *
 * 板级资源（DMA 乒乓双缓冲、RE 方向控制）由 Device 层 dev_rs485 提供，
 * 通道生命周期和收发任务循环全部在 Application 层实现。
 *
 * RX 路径：pl_uart circular 乒乓（2×640B）经 HT/TC/IDLE 中断增量通知，
 * ISR 回调把「块号 + 偏移 + 长度」消息放入 rx_queue（深度 2），
 * 任务按消息从对应块拷贝到 RB（app_channel_dispatch）。
 */

#include "app_rs485.h"

#include "FreeRTOS.h"
#include "pl_uart.h"
#include "dev_rs485.h"
#include "app_dispatch.h"

typedef struct {
    channel_t me;
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    uint8_t *rx_buf;
    uint16_t rx_buf_size;   /* 总长 = 2 × block（circular 周期） */
    uint16_t rx_block_size; /* 半块 = RS485_BUF_SIZE */
} rs485_ch_t;

/* ---- rs485_rx_queue 静态分配（深度 2：乒乓双块各一消息在途） ---- */
static StaticQueue_t s_rs485_rx_cb;
static pl_uart_rx_msg_t s_rs485_rx_buf[2];
static const osMessageQueueAttr_t s_rs485_rx_attr = {
    .name    = "rs485_rx",
    .cb_mem  = &s_rs485_rx_cb,
    .cb_size = sizeof(s_rs485_rx_cb),
    .mq_mem  = s_rs485_rx_buf,
    .mq_size = sizeof(s_rs485_rx_buf),
};

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
    rs485_ch_t *self = (rs485_ch_t *)ctx;
    uint32_t off      = (uint32_t)(data - self->rx_buf);

    /* 段指针换算为「块号 + 块内偏移」；len 由 pl_uart 保证 ≤ block */
    pl_uart_rx_msg_t m = {
        .block  = (uint8_t)(off / self->rx_block_size),
        .offset = (uint16_t)(off % self->rx_block_size),
        .len    = len,
    };
    osMessageQueuePut(self->rx_queue, &m, 0, 0);
}

/* ---- 任务循环 ---- */
static void rs485_task(void *argument)
{
    rs485_ch_t *self = (rs485_ch_t *)argument;

    self->rx_queue = osMessageQueueNew(2, sizeof(pl_uart_rx_msg_t), &s_rs485_rx_attr);
    if (self->rx_queue == NULL) {
        osThreadExit();
        return;
    }
    app_channel_register(self->me.ch_id, &self->me);

    pl_uart_set_rx_cb(self->uart, rs485_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);

    for (;;) {
        pl_uart_rx_msg_t m;
        if (osMessageQueueGet(self->rx_queue, &m, 0, osWaitForever) == osOK) {
            /* 按块号从对应缓冲拷贝 len 字节入调度框架 */
            uint8_t *src = self->rx_buf + (uint16_t)m.block * self->rx_block_size + m.offset;
            app_channel_dispatch(&self->me, src, m.len);
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
    self->rx_block_size = RS485_BUF_SIZE;
    self->rx_buf_size   = 2U * RS485_BUF_SIZE; /* 乒乓双块：640→1280B */

    osThreadAttr_t rs485_task_attr = {
        .name       = "rs485_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    return osThreadNew(rs485_task, self, &rs485_task_attr);
}