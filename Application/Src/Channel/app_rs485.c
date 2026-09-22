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
#include <string.h> /* memcpy：ISR 把收到的这一段拷进自己的槽位 */

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
/* ================================================================
 *  收包槽位 —— **ISR 把这一段拷出来再投递**
 *
 * 原先队列里只放"长度"，数据留在共享的 rx_buf 里，深度只能是 1：
 *   · 队列一旦满，投递失败 → **这一段静默丢掉**（Put 超时为 0）；
 *   · 队列加深也没用 —— 数据在共享缓冲里，第二个槽的"长度"会配上已经被覆盖的
 *     "数据"，比丢更糟。
 *
 * 实测就是这样丢的：从卡在 tick 3457 收到了 COMMIT 那个 15 字节的块，ISR 那一行打了，
 * 而"交付给任务"那一行**根本没有** —— 投递失败。之后它在环形缓冲区里被下一个块
 * 顺带带进去，于是 21 秒后才被处理。
 *
 * 改成"ISR 拷进自己的槽位、队列里放槽号"：投递不再会丢，也不会错配。
 * 2 槽足以吸收一次调度延迟（分片是 1ms 一帧连发的）。
 * 放 CCMRAM：这是 ISR 里 memcpy 的目的地，CPU 访问，不需要 DMA 可达。 */
/* ---- 收包槽位 ----
 *
 * **3 个**（原来 2 个）：`uart_idle_handle` 在**缓冲区环回处**会把一段拆成两次**背靠背**的
 * 回调（先交缓冲末尾那一截、再交开头那一截）。也就是"一次交付"最多同时占 2 个槽，
 * 而任务手上最多还握着 1 个 —— 2 个槽在环回那一刻必然不够。实测：一条 1427 字节的帧
 * 跨过回绕点被拆成 1211 + 216，后半段**被丢掉**，整帧作废，只能靠主卡重发。
 * 3 个刚好覆盖这个最坏情形（1 个在任务手上 + 2 个环回拆出的段）。
 *
 * **放 SRAM 而不是 CCMRAM**（原来在 CCMRAM）：它是 ISR 的 memcpy 目的地，CPU 访问即可，
 * 速度上只要"跟得上 115200"——11520 B/s，从哪块 RAM 拷都绰绰有余。而 CCMRAM 是全工程
 * 最紧的资源（一帧一轮之后也只剩 3.2KB），槽位一挪就换出 4KB 给真正需要它的地方。 */
#define RS485_RX_SLOTS (3U)

typedef struct {
    uint16_t len;
    uint8_t  data[RS485_BUF_SIZE];
} rs485_rx_slot_t;

static rs485_rx_slot_t s_slots[RS485_RX_SLOTS];
static uint8_t         s_slot_next; /* ISR 下次写哪个槽 */
static uint8_t         s_slot_busy; /* 位 i = 槽 i 已投递、尚未被任务取走 */

/** @brief 副本长度上限的一个静态护栏（投递的是槽号，不是长度） */
typedef struct {
    uint8_t slot;
} rs485_rx_msg_t;

static StaticQueue_t  s_rs485_rx_cb;
static rs485_rx_msg_t s_rs485_rx_buf[RS485_RX_SLOTS];
static const osMessageQueueAttr_t s_rs485_rx_attr = {
    .name    = "rs485_rx",
    .cb_mem  = &s_rs485_rx_cb,
    .cb_size = sizeof(s_rs485_rx_cb),
    .mq_mem  = s_rs485_rx_buf,
    .mq_size = sizeof(s_rs485_rx_buf),
};

/* ---- 传输层收包诊断 ----
 *
 * **正常运行时静默**（`RS485_RX_LOG` 置 0）：它每交付一段就打一行，1KB 的 RTT 缓冲
 * 会被它占掉大半。**查"帧到没到"时把它置 1 是决定性的** —— "收到 N 字节"与"什么都没
 * 收到"把问题一刀切成"上游（线/收发器/接收 DMA）"与"下游（分发/探针/匹配）"，
 * 两者排查方向相反；顺带还能看出对端的分帧（一个块是不是几条帧粘出来的）。 */
#define RS485_RX_LOG     0
#define RS485_RX_LOG_MAX 60U

/* 异常另算：**这两条无论开关如何都要报**（限次），因为它们是"静默丢数据"的仅有的声音 ——
   `槽位用尽` = 任务落后到没槽可放，`投递队列满` = 通知投不进去。两者都丢一段，
   而丢了不会有人重发（只能等对端超时重传），所以必须看得见；健康时它们一次都不出现。 */
#define RS485_ANOMALY_LOG_MAX 8U

static void rs485_isr_cb(uint8_t *data, uint16_t len, void *ctx)
{
    (void)data;
    rs485_ccb_t *self = (rs485_ccb_t *)ctx;

    if (len > RS485_BUF_SIZE) return; /* 不可能：DMA 缓冲就这么大 */

    /* 找一个空槽；全占着说明任务已经落后整整一段，这一段落掉（并报出来） */
    uint8_t slot = 0xFF;
    for (uint8_t k = 0; k < RS485_RX_SLOTS; k++) {
        const uint8_t i = (uint8_t)((s_slot_next + k) % RS485_RX_SLOTS);
        if (!(s_slot_busy & (uint8_t)(1U << i))) { slot = i; break; }
    }
    if (slot == 0xFF) {
        static uint8_t s_full_logged;
        if (s_full_logged < RS485_ANOMALY_LOG_MAX) {
            s_full_logged++;
            printf("[%8u] [rs485] **槽位用尽，丢一段 %u 字节**\n",
                   (unsigned)osKernelGetTickCount(), (unsigned)len);
        }
        return;
    }

    memcpy(s_slots[slot].data, data, len);
    s_slots[slot].len = len;
    s_slot_busy |= (uint8_t)(1U << slot);
    s_slot_next = (uint8_t)((slot + 1U) % RS485_RX_SLOTS);

    const rs485_rx_msg_t m = {.slot = slot};
    if (osMessageQueuePut(self->rx_queue, &m, 0, 0) != osOK) {
        /* **投不进队列就必须把槽还回去**：队列满时 Put 会失败（超时 0），
           而 busy 位已经置上 —— 标着却没人会来取，那个槽就**永久**漏了。
           漏两个就再也收不到任何东西，且一声不响（实测发生过：日志里
           "某一段没有对应的交付行"就是它）。这一段落掉即可：对端会重发。 */
        s_slot_busy &= (uint8_t)~(1U << slot);
        static uint8_t s_qfull_logged;
        if (s_qfull_logged < RS485_ANOMALY_LOG_MAX) {
            s_qfull_logged++;
            printf("[%8u] [rs485] **投递队列满，丢一段 %u 字节**（槽已归还）\n",
                   (unsigned)osKernelGetTickCount(), (unsigned)len);
        }
    }
}

/* ---- 任务循环 ---- */

/** @brief 建收包队列 —— **深度必须等于槽数**，理由见 rs485_isr_cb 里那段说明
 *
 *  单独一个函数是为了让 host 测试能走**同一条路**：测试若自己另建一个队列，
 *  改坏了这里（比如把深度写小）它一声不响 —— 那就成了假信心。 */
static osMessageQueueId_t _rx_queue_create(void)
{
    return osMessageQueueNew(RS485_RX_SLOTS, sizeof(rs485_rx_msg_t), &s_rs485_rx_attr);
}

static void rs485_task(void *argument)
{
    rs485_ccb_t *self = (rs485_ccb_t *)argument;

    self->rx_queue = _rx_queue_create();
    if (self->rx_queue == NULL) {
        osThreadExit();
        return;
    }

    pl_uart_set_rx_cb(self->uart, rs485_isr_cb, self);
    pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);
    self->base.state = CCB_STATE_UP;

    for (;;) {
        rs485_rx_msg_t m = {0};
        if (osMessageQueueGet(self->rx_queue, &m, 0, osWaitForever) == osOK) {
            const uint16_t n = s_slots[m.slot].len;
#if RS485_RX_LOG
            printf("[%8u] [rs485] 交付槽 %u：%u 字节\n", (unsigned)osKernelGetTickCount(),
                   (unsigned)m.slot, (unsigned)n);
#endif
            (void)n;
            app_ccb_dispatch(&self->base, nullptr, s_slots[m.slot].data, n);
            s_slot_busy &= (uint8_t)~(1U << m.slot); /* 交出去之后才能释放，否则 ISR 会覆写 */
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
     * 慢一拍就多占一格槽位。槽位只有 RS485_RX_SLOTS 个，满了两端都要丢东西
     * （ISR 丢新段、或投递失败把那一段丢在槽里）—— 而现场表现为"总缺中间某一片"
     * 或"某张卡整轮不答"，两侧都不会报错。
     *
     * （早先这里是"共享 rx_buf + 深 1 的长度队列"，那时队列**不能**加深：长度与数据
     * 会串。现在每段都拷进**自己的槽位**、队列投的是槽号，两者一一对应 ——
     * 所以深度跟着槽数走是安全且必需的，见 rs485_isr_cb 里的说明。）
     *
     * 提到分发任务之上后，ISR 投递完立刻抢占执行。本任务只做 rb_write + 通知，很短。 */
    osThreadAttr_t rs485_task_attr = {
        .name       = "rs485_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityAboveNormal,
    };
    return pl_task_new(rs485_task, self, &rs485_task_attr);
}
