/**
 * @file    app_rs485.c
 * @brief   RS485 半双工通道（USART1/PL_UART1, RE 方向由 dev_rs485 注入）
 *
 * 板级资源（DMA 缓冲区、RE 方向控制）由 Device 层 dev_rs485 提供，
 * 通道生命周期和收发任务循环全部在 Application 层实现。
 *
 * 通道控制块为静态对象，与连接状态无关：协议绑定期间即有效，
 * 任务启动只填连接相关字段。
 */

#include "app_rs485.h"

#include "FreeRTOS.h"
#include "pl_uart.h"
#include "pl_mem.h"
#include "dev_rs485.h"
#include <stdio.h>

#include "app_dispatch.h"
#include "pl_task.h"

#define RS485_BUF_SIZE (2048U)

/* 发送超时。**不必在这里算字节数** —— pl_uart 会按波特率兜下限
   （原实现把 100ms 写死在这里，@115200 只够 1152 字节，长帧会被 HAL 拦腰截断）。
   给一个宽松值即可，真正的下限由 pl_uart 抬。 */
#define RS485_TX_TIMEOUT_MS (200U)

typedef struct {
    ccb_t base; /**< 第一个成员：container_of 还原 */
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
} rs485_ccb_t;

/* ---- ops ---- */
static int32_t rs485_send(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)dst; /* 半双工总线：目的地恒为总线对端，无广播/寻址概念，请求一律忽略 */
    rs485_ccb_t *self = container_of(ccb, rs485_ccb_t, base);
    /* state 置 UP 的唯一位置在任务里、UART 与 DMA 接收就绪之后，
       因此它同时表达了"uart 已绑定"，无需再单独判空 */
    if (self->base.state != CCB_STATE_UP) return -1;

    /* 长帧走 DMA：轮询会把调用任务按在整帧的物理时间上（1.4KB @115200 = 122ms），
       DMA 下这段时间交给硬件、任务阻塞在信号量上。
       **但缓冲落在 CCMRAM 时只能轮询** —— DMA 够不到 CCM 区（见 pl_mem.h），
       这是数据摆放决定的、不是可选项。在这里先判，免得 pl_uart 每次都打一条拒绝日志，
       也免得协议侧拿 CCMRAM 缓冲发响应时被静默丢弃（返回值无人检查）。 */
    if (pl_mem_is_dma_capable(data, len))
        return pl_uart_send_dma(self->uart, data, len, RS485_TX_TIMEOUT_MS);
    return pl_uart_send(self->uart, data, len, RS485_TX_TIMEOUT_MS);
}

static const ccb_ops_t rs485_ccb_ops = {.send = rs485_send};

/* ---- 通道控制块（静态，协议绑定期间即可用） ---- */
static rs485_ccb_t g_rs485 = {
    .base = {.name = "rs485", .ops = &rs485_ccb_ops},
};

ccb_t *app_rs485_ccb(void)
{
    return &g_rs485.base;
}

/* ---- rs485_rx_queue 静态分配 ---- */
static StaticQueue_t s_rs485_rx_cb;
static uint16_t s_rs485_rx_buf[1];
static const osMessageQueueAttr_t s_rs485_rx_attr = {
    .name    = "rs485_rx",
    .cb_mem  = &s_rs485_rx_cb,
    .cb_size = sizeof(s_rs485_rx_cb),
    .mq_mem  = s_rs485_rx_buf,
    .mq_size = sizeof(s_rs485_rx_buf),
};

/* ---- 传输层收包诊断 ----
 *
 * 查"帧到没到"时它是决定性的：**收到 527 字节**与**什么都没收到**，把问题一刀切成
 * "上游（线/收发器/接收 DMA）"与"下游（分发/探针/匹配）"—— 两者排查方向相反。
 * 顺带能看出对端的**分帧**（一个块是几条帧粘出来的）。
 *
 * **限次**：一次运行只报前 RS485_RX_LOG_MAX 条，不然会把 1KB 的 RTT 缓冲冲掉，
 * 反而看不到别的。查完把它置 0 关掉。 */
/* **当前关着**：级联那轮已经查完（传输层的使命完成了），而它每条块打一行会把
   1KB 的 RTT 缓冲占掉大半 —— 级联自己的日志更要紧。再查"帧到没到"时置 1 即可。 */
#define RS485_RX_LOG 0
#define RS485_RX_LOG_MAX 20U

static void rs485_isr_cb(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    rs485_ccb_t *self = (rs485_ccb_t *)ctx;
#if RS485_RX_LOG
    static uint8_t s_logged;
    if (s_logged < RS485_RX_LOG_MAX) {
        s_logged++;
        printf("[rs485] 收到 %u 字节\n", (unsigned)len);
    }
#endif
    osMessageQueuePut(self->rx_queue, &len, 0, 0);
}

/* ---- 任务循环 ---- */
static void rs485_task(void *argument)
{
    rs485_ccb_t *self = (rs485_ccb_t *)argument;

    self->rx_queue = osMessageQueueNew(1, sizeof(uint16_t), &s_rs485_rx_attr);
    if (self->rx_queue == NULL) {
        osThreadExit();
        return;
    }

    pl_uart_set_rx_cb(self->uart, rs485_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);
    self->base.state = CCB_STATE_UP;

    for (;;) {
        uint16_t rx_len = 0;
        if (osMessageQueueGet(self->rx_queue, &rx_len, 0, osWaitForever) == osOK) {
            app_ccb_dispatch(&self->base, nullptr, self->rx_buf, rx_len);
        }
    }
}

/* ---- 公开 API ---- */

osThreadId_t app_rs485_start(void)
{
    rs485_ccb_t *self  = &g_rs485;
    self->uart        = pl_uart_get_handle(PL_UART1);
    self->rx_buf      = dev_rs485_get_buf();
    self->rx_buf_size = RS485_BUF_SIZE;

    /* **优先级高于分发任务**（Normal → AboveNormal）。
     *
     * 这一级是整条接收链上唯一**没有缓冲余量**的：ISR 把"本段的长度"投进一个深 1
     * 的队列，数据还在共享的 rx_buf 里 —— 队列加深会把"上一段的长度"配到"下一段的
     * 数据"上，比丢更糟。而分片是 1ms 一帧连发的，本任务若等到下一个 tick 才被调度，
     * 那一格就是满的，下一段**静默丢弃**：现场表现为"总缺中间某一片"或"某张卡整轮
     * 不答"（COMMIT 被丢了），而两侧都不会报错。
     *
     * 提到分发任务之上后，ISR 投递完立刻抢占执行，这一格永远是空的。
     * 本任务只做 rb_write + 通知，很短，不会饿着别人。 */
    osThreadAttr_t rs485_task_attr = {
        .name       = "rs485_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityAboveNormal,
    };
    return pl_task_new(rs485_task, self, &rs485_task_attr);
}
