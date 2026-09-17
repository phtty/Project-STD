/**
 * @file    app_rs232.c
 * @brief   RS232 通道（RS232-0 = USART3/PL_UART3, RS232-1 = USART6/PL_UART6）
 *
 * 板级资源（DMA 缓冲区）由 Device 层 dev_rs232 提供，
 * 通道生命周期和收发任务循环全部在 Application 层实现。
 *
 * 两路共用同一套 ops 与任务循环，差异只在实例字段（UART 句柄、DMA 缓冲、
 * 接收通知队列属性、任务属性）—— 由各自的 start 函数在启动前填好。
 * 控制块为静态对象，协议绑定期间即有效。
 */

#include "app_rs232.h"

#include "FreeRTOS.h"
#include "pl_uart.h"
#include "dev_rs232.h"
#include "app_dispatch.h"
#include "pl_task.h"

#define RS232_BUF_SIZE (2048U)

typedef struct {
    ccb_t base; /**< 第一个成员：container_of 还原 */
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
    const osMessageQueueAttr_t *rx_attr; /**< 本实例的接收通知队列属性（静态分配）*/
} rs232_ccb_t;

/* ---- ops（两路共用：目的地恒为本 UART 对端，无寻址概念） ---- */
static int32_t rs232_send(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)dst;
    rs232_ccb_t *self = container_of(ccb, rs232_ccb_t, base);
    /* state 置 UP 的唯一位置在任务里、UART 与 DMA 接收就绪之后，
       因此它同时表达了"uart 已绑定"，无需再单独判空 */
    if (self->base.state != CCB_STATE_UP) return -1;
    return pl_uart_send(self->uart, data, len, 100);
}

static const ccb_ops_t rs232_ccb_ops = {.send = rs232_send};

/* ---- 通道控制块（静态，协议绑定期间即可用） ---- */
static rs232_ccb_t g_rs232_0 = {
    .base = {.name = "rs232_0", .ops = &rs232_ccb_ops},
};
static rs232_ccb_t g_rs232_1 = {
    .base = {.name = "rs232_1", .ops = &rs232_ccb_ops},
};

ccb_t *app_rs232_0_ccb(void)
{
    return &g_rs232_0.base;
}

ccb_t *app_rs232_1_ccb(void)
{
    return &g_rs232_1.base;
}

/* ---- rs232_rx_queue 静态分配 ---- */
static StaticQueue_t s_rs232_0_rx_cb;
static uint16_t s_rs232_0_rx_buf[1];
static const osMessageQueueAttr_t s_rs232_0_rx_attr = {
    .name    = "rs232_0_rx",
    .cb_mem  = &s_rs232_0_rx_cb,
    .cb_size = sizeof(s_rs232_0_rx_cb),
    .mq_mem  = s_rs232_0_rx_buf,
    .mq_size = sizeof(s_rs232_0_rx_buf),
};

static StaticQueue_t s_rs232_1_rx_cb;
static uint16_t s_rs232_1_rx_buf[1];
static const osMessageQueueAttr_t s_rs232_1_rx_attr = {
    .name    = "rs232_1_rx",
    .cb_mem  = &s_rs232_1_rx_cb,
    .cb_size = sizeof(s_rs232_1_rx_cb),
    .mq_mem  = s_rs232_1_rx_buf,
    .mq_size = sizeof(s_rs232_1_rx_buf),
};

/* ---- rs232 task attr ---- */
static const osThreadAttr_t s_rs232_0_attr = {
    .name       = "rs232_0_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
static const osThreadAttr_t s_rs232_1_attr = {
    .name       = "rs232_1_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- ISR → 任务通知 ---- */
static void rs232_isr_cb(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    rs232_ccb_t *self = (rs232_ccb_t *)ctx;
    osMessageQueuePut(self->rx_queue, &len, 0, 0);
}

/* ---- 任务循环（两路共用） ---- */
static void rs232_task(void *argument)
{
    rs232_ccb_t *self = (rs232_ccb_t *)argument;

    self->rx_queue = osMessageQueueNew(1, sizeof(uint16_t), self->rx_attr);
    if (self->rx_queue == NULL) {
        osThreadExit();
        return;
    }

    pl_uart_set_rx_cb(self->uart, rs232_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);
    self->base.state = CCB_STATE_UP;

    for (;;) {
        uint16_t rx_len = 0;
        osMessageQueueGet(self->rx_queue, &rx_len, 0, osWaitForever);
        app_ccb_dispatch(&self->base, nullptr, self->rx_buf, rx_len);
    }
}

/* ---- 公开 API ---- */

static osThreadId_t rs232_start(rs232_ccb_t *self, pl_uart_handle_t uart, uint8_t buf_index,
                                const osThreadAttr_t *attr, const osMessageQueueAttr_t *rx_attr)
{
    self->uart        = uart;
    self->rx_buf      = dev_rs232_get_buf(buf_index);
    self->rx_buf_size = RS232_BUF_SIZE;
    self->rx_attr     = rx_attr;
    return pl_task_new(rs232_task, self, attr);
}

osThreadId_t app_rs232_start(void)
{
    return rs232_start(&g_rs232_0, pl_uart_get_handle(PL_UART3), 0, &s_rs232_0_attr,
                       &s_rs232_0_rx_attr);
}

osThreadId_t app_rs232_1_start(void)
{
    return rs232_start(&g_rs232_1, pl_uart_get_handle(PL_UART6), 1, &s_rs232_1_attr,
                       &s_rs232_1_rx_attr);
}
