/**
 * @file        app_dispatch.c
 * @brief       协议调度框架实现（Application 层核心）
 *
 * 数据流（接收路径）:
 *   物理接口 → 通道任务 → app_ccb_dispatch() → 各协议自有环形缓冲区 + ccb_queue
 *       → frame_dispatch_task() → 协议探测 → 协议帧队列 → 协议处理任务
 *
 * 通道发送通过 ccb_ops 虚表分派（OCP 模式），不依赖具体传输实现。
 *
 * 每个协议持有自己的缓冲区，因此一个缓冲区只有唯一主人：探测时不存在协议争用，
 * 框架不需要协议优先级，也不会出现跨协议/跨通道的数据污染。
 */

#include "app_dispatch.h"

#include "FreeRTOS.h"
#include "task.h" /* configASSERT 用到 taskDISABLE_INTERRUPTS */
#include "cmsis_os2.h"
#include "initcall.h"

#include <string.h>
#include "pl_task.h"

/* frame_msg_t 的 data 必须 4 字节对齐：探针会把 scratch 直接 cast 成
 * uint32 字段的帧结构体（如 IAP）访问。 */
static_assert(offsetof(frame_msg_t, data) % 4 == 0, "frame_msg_t.data 必须 4 字节对齐");

/* ================================================================
 *  通道通知队列 — 编译期静态分配
 * ================================================================ */

/* 通知元素：通道指针 + 本批字节数 + 来源副本。
 *
 * 三件事一次解决：
 *   1. 来源**按值拷贝**，归框架所有 —— 不受通道侧缓冲被后续派发覆盖的影响；
 *   2. 记下本批字节数，排空按量计量 —— 先到的那批不会把后到者的字节算进自己的来源。
 *
 *   反方向没有保证：先到者未排完的余量会由后到者的来源解析（补齐需要"挂起
 *   来源"状态，未实现）。完整说明见 app_dispatch.h 里 ccb_src_t 的归属一节。 */
typedef struct {
    ccb_t *ccb;
    uint16_t len;                  /**< 本次派发写入的字节数 */
    char topic[CCB_SRC_TOPIC_MAX]; /**< 来源副本；空串表示无来源 */
} ccb_notify_t;

static StaticQueue_t s_ccb_queue_cb;
static ccb_notify_t s_ccb_queue_buf[CCB_NOTIFY_MAX];
static const osMessageQueueAttr_t s_ccb_queue_attr = {
    .name    = "g_ccb_queue",
    .cb_mem  = &s_ccb_queue_cb,
    .cb_size = sizeof(s_ccb_queue_cb),
    .mq_mem  = s_ccb_queue_buf,
    .mq_size = sizeof(s_ccb_queue_buf),
};

/* ================================================================
 *  帧暂存区 — 兼作探针 scratch
 *
 *  探针窥视的目标与入队缓冲是同一块内存：探针把帧窥视到 msg->data，
 *  READY 后框架再 rb_read 到同一位置（内容本就相同，等于一次冗余拷贝）。
 *  这样既省掉每个协议一份专用 scratch，又不依赖"探针一定窥视过完整帧"
 *  这一约定 —— 最终入队的内容只由 rb_read 决定。
 * ================================================================ */

static uint8_t _msg_dispatch_buf[sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN]
    __attribute__((aligned(4)));

/* ================================================================
 *  调度上下文
 * ================================================================ */

dispatch_ctx_t g_dispatch;           /**< 全局调度上下文 */
osThreadId_t g_dispatch_task_handle; /**< 帧分发任务句柄（外部用于 Suspend/Resume） */

/** 接收事件监听：框架不直接依赖任何业务模块，由模块自行注册 */
static dispatch_rx_listener_t s_rx_listener;

void app_dispatch_register_rx_listener(dispatch_rx_listener_t fn)
{
    s_rx_listener = fn;
}

/* ================================================================
 *  协议绑定 — 协议模块通过 sw_app_initcall 自注册
 *
 *  协议在自己的 init 里填好 pcb 字段（name/ops/rb/payload_max，建好 queue），
 *  然后对每个承载通道调用一次 app_proto_bind 即完成注册：
 *      app_proto_bind(&s_ldi_pcb, app_tcp_server_ccb());
 *
 *  绑定只写 ccb->protos[]，而 frame_dispatch_task 只读它；只要所有绑定发生在
 *  任何通道任务启动之前（sw_board_init 早于各 app_xxx_start）即天然安全。
 * ================================================================ */

void app_proto_bind(pcb_t *pcb, ccb_t *ccb)
{
    if (pcb == nullptr || ccb == nullptr) return;

    /* 重复绑定忽略 */
    for (uint8_t i = 0; i < ccb->proto_cnt; i++)
        if (ccb->protos[i] == pcb) return;

    if (ccb->proto_cnt >= CCB_PROTO_MAX) {
        configASSERT(0); /* 槽位不足：调大 CCB_PROTO_MAX */
        return;
    }
    ccb->protos[ccb->proto_cnt++] = pcb;
}

/* ================================================================
 *  调度系统初始化 — sw_app_initcall，RTOS 后自动调用
 * ================================================================ */

void app_dispatch_init(void)
{
    g_dispatch.ccb_queue = osMessageQueueNew(CCB_NOTIFY_MAX, sizeof(ccb_notify_t), &s_ccb_queue_attr);

    const osThreadAttr_t frame_dispatch_task_attr = {
        .name       = "frame_dispatch_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    g_dispatch_task_handle = pl_task_new(frame_dispatch_task, nullptr, &frame_dispatch_task_attr);
}
sw_app_initcall(app_dispatch_init);

/* ================================================================
 *  frame_dispatch_task — 帧分发引擎（核心调度循环）
 *
 *  流程:
 *    1. 阻塞等待 ccb_queue 中的通道指针通知
 *    2. 遍历该通道承载的协议，每个协议持有自己的环形缓冲区
 *    3. 持锁跨整轮"探测 + 读取"，消除 TOCTOU 窗口
 *    4. 一次通知可能触发多帧解析 (while avail > 0)
 *
 *  因为每个缓冲区只有唯一的主人，探测不必按优先级依次询问多个协议，
 *  读取结果也只由该协议自己的探针决定。
 * ================================================================ */

void frame_dispatch_task(void *argument)
{
    (void)argument;

    ccb_notify_t notify;
    frame_msg_t *msg = (frame_msg_t *)_msg_dispatch_buf;

    for (;;) {
        if (osMessageQueueGet(g_dispatch.ccb_queue, &notify, NULL, osWaitForever) != osOK)
            continue;

        ccb_t *ccb = notify.ccb;
        if (ccb == nullptr) continue;

        /* 探针看到的来源指向通知元素里的副本（框架自有存储，本次排空内稳定）*/
        const ccb_src_t src   = {.topic = (notify.topic[0] != '\0') ? notify.topic : nullptr};
        const ccb_src_t *psrc = (src.topic != nullptr) ? &src : nullptr;

        for (uint8_t i = 0; i < ccb->proto_cnt; i++) {
            pcb_t *p = ccb->protos[i];
            if (p == nullptr || p->ops == nullptr || p->ops->probe == nullptr) continue;
            if (p->rb == nullptr || p->queue == nullptr) continue;

            /* 持锁跨整轮探测+读取，消除 TOCTOU */
            rb_lock(p->rb);

            /* 只消费本次派发写入的字节：否则先到的那批会把后到者的字节也算进
               自己的来源里。反方向不保证 —— 本批未排完的余量（半帧）留在缓冲
               区，会由后到者的来源解析；补齐需要"挂起来源"状态，未实现，
               影响面见 app_dispatch.h 里 ccb_src_t 的归属一节。 */
            uint32_t budget = notify.len;

            while (budget > 0 && rb_avail(p->rb, nullptr) > 0) {
                uint32_t frame_len = 0;
                uint8_t aux        = 0;
                uint16_t before    = rb_avail(p->rb, nullptr);

                /* 调用探测函数（调用者持锁，探测内部传 nullptr 跳过锁） */
                pcb_probe_sta_t state = p->ops->probe(p, ccb, psrc, msg->data,
                                                      FRAME_DATA_MAX_LEN, &frame_len, &aux);

                if (state == PCB_PROBE_WAIT) break; /* 数据不足：等下一批数据 */

                if (state == PCB_PROBE_READY) {
                    /* 探针契约：0 < frame_len ≤ payload_max ≤ FRAME_DATA_MAX_LEN。
                     * 违规时丢弃 1 字节并继续，避免零进度死循环。 */
                    if (frame_len == 0 || frame_len > p->payload_max ||
                        frame_len > FRAME_DATA_MAX_LEN) {
                        rb_skip(p->rb, 1, nullptr);
                        goto account; /* 违规帧：丢弃 1 字节，避免零进度死循环 */
                    }
                    /* 读出长度以 rb_read 的返回值为准（与窥视内容一致） */
                    uint16_t actual = rb_read(p->rb, msg->data, (uint16_t)frame_len, nullptr);
                    if (actual != frame_len)
                        goto account; /* 已消费 actual 字节，无零进度风险 */

                    msg->data_len = (uint16_t)frame_len;
                    msg->aux      = aux;
                    msg->ccb      = ccb;
                    osMessageQueuePut(p->queue, msg, 0, 0);
                    goto account;
                }

                /* SKIP: 帧结构合法但不属于本设备 → 跳过整帧
                 * FAKE: 伪帧头 → 跳过 1 字节重试 */
                uint16_t skip =
                    (state == PCB_PROBE_SKIP && frame_len > 0) ? (uint16_t)frame_len : 1;
                rb_skip(p->rb, skip, nullptr);

            account: {
                uint16_t used = (uint16_t)(before - rb_avail(p->rb, nullptr));
                budget        = (used >= budget) ? 0 : budget - used;
            }
            }

            rb_unlock(p->rb);
        }
    }
}

/* ================================================================
 *  ccb_send / ccb_send_to — 通道发送（OCP：虚表分派）
 *
 *  协议把数据交给通道，目的地用 ccb_dst_t 表达：nullptr = 回复到本帧来源，
 *  否则由通道解释自己能认识的字段（broadcast / topic），其余忽略并退化为
 *  默认行为。协议只表达意图，通道翻译成自己的机制 —— 新增通道类型无需改此处，
 *  协议也不必认识具体通道类型。
 *
 *  安全守卫：ops / ops->send 为空表示通道尚未就绪，直接丢弃。
 * ================================================================ */

void ccb_send_to(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    if (ccb == nullptr || ccb->ops == nullptr || ccb->ops->send == nullptr) return;
    ccb->ops->send(ccb, dst, data, len);
}

void ccb_send(ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    ccb_send_to(ccb, nullptr, data, len);
}

/* ================================================================
 *  app_ccb_dispatch — 通道接收分发
 *
 *  所有通道任务的接收路径统一入口：
 *    1. 遍历该通道承载的协议，各自写入自己的环形缓冲区
 *    2. 触发接收事件监听（工厂模式等由监听者自行注册）
 *    3. 向 ccb_queue 发送通道指针通知帧分发任务
 *
 *  每个协议持有独立缓冲区，故无需去重。某一路装不下时按"丢旧留新"处理：
 *  缓冲区里剩下的只可能是不完整的半截帧，留着它去毒化后续解析比丢掉更糟。
 *
 *  注意：通道任务不要直接操作 ring buffer 或 mutex。
 * ================================================================ */

void app_ccb_dispatch(const ccb_t *ccb, const ccb_src_t *src, const uint8_t *data,
                      uint16_t len)
{
    if (ccb == nullptr || data == nullptr || len == 0) return;

    for (uint8_t i = 0; i < ccb->proto_cnt; i++) {
        ring_buffer_t *rb = ccb->protos[i]->rb;
        if (rb == nullptr) continue;

        /* 装不下就整段丢弃旧数据重来（"新数据优先"）：缓冲区里剩下的只可能是不完整的
           半截帧，留着它去毒化后续解析（探针只能逐字节重新同步，还可能同步到错误位置）
           比直接丢掉更糟。持锁完成"判断 + 清空 + 写入"，避免与分发任务竞争。
           注：len 超过缓冲区总容量时怎么都装不下，那属于 RB 定容错误 ——
           RB 容量必须 ≥ 传输层单次最大写入。 */
        rb_lock(rb);
        if (rb_space(rb, nullptr) < len) rb_flush(rb, nullptr);
        rb_write(rb, data, len, nullptr);
        rb_unlock(rb);
    }

    if (s_rx_listener) s_rx_listener();

    /* 通知帧分发任务：来源在此拷进框架自有存储，调用方的指针随后即可失效 */
    ccb_notify_t notify = {.ccb = (ccb_t *)ccb, .len = len, .topic = {0}};
    if (src != nullptr && src->topic != nullptr)
        strncpy(notify.topic, src->topic, sizeof(notify.topic) - 1);

    osMessageQueuePut(g_dispatch.ccb_queue, &notify, 0, 0);
}
