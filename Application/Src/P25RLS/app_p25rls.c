/**
 * @file    app_p25rls.c
 * @brief   P25RLS 协议主实现：帧探测、消息任务与命令分派
 *
 * 帧格式与 RLS 相同，但两者**不能同时编进一块板**（见 app_p25rls.h 的说明）。
 * 通道只用 UDP：协议文档 §2 给的是 UDP/IP（设备 10011、上位机 10028），
 * §1 那句"通过 RS232 接口控制"是产品描述里的旧话，本产品按 UDP 实现。
 */

#include "app_p25rls.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "initcall.h"

#include "app_p25rls_cmd.h"
#include "app_udp.h"
#include "pl_mem.h"
#include "pl_task.h"

/* ---- 队列与消息缓冲 ----
 * 元素 = 帧消息头 + 最长帧；两块在 CCMRAM（与 CDSTD/GZZH 同一取值口径）。 */
#define P25RLS_MSG_BYTES (sizeof(app_dispatch_msg_t) + P25RLS_FRAME_MAX_BYTES)

static StaticQueue_t s_p25rls_queue_cb;
static uint8_t s_p25rls_queue_buf[2 * P25RLS_MSG_BYTES] PL_CCMRAM;
static const osMessageQueueAttr_t s_p25rls_queue_attr = {
    .name    = "proto_p25rls_queue",
    .cb_mem  = &s_p25rls_queue_cb,
    .cb_size = sizeof(s_p25rls_queue_cb),
    .mq_mem  = s_p25rls_queue_buf,
    .mq_size = sizeof(s_p25rls_queue_buf),
};

/** @brief 任务取帧用的接收缓冲（比队列元素多一份，避免在任务栈上摊 1.4KB） */
static uint8_t s_p25rls_msg_buf[P25RLS_MSG_BYTES];

static osMessageQueueId_t s_p25rls_msg_queue;
static osThreadId_t s_p25rls_task_handle;
static const osThreadAttr_t s_p25rls_task_attr = {
    .name       = "app_p25rls_task",
    .stack_size = 384 * 4, /* 命令层要跑字库渲染与位图三遍绘制 */
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ---- 协议控制块 ----
 * 环形缓冲区容量：`max(2 × 最长帧 1440, 单次最大写入 2048 + 1)`。**必须严格大于
 * 单次最大写入**：ring buffer 用保留一个空槽区分满/空，取相等值时满的那一次会
 * 静默截掉末字节。 */
RB_DEFINE_ATTR(s_p25rls_rb, 2944, PL_CCMRAM);

static const app_pcb_ops_t s_p25rls_ops = {.probe = app_p25rls_probe_frame};

static app_pcb_t s_p25rls_pcb = {
    .name        = "p25rls",
    .ops         = &s_p25rls_ops,
    .rb          = &s_p25rls_rb,
    .payload_max = P25RLS_FRAME_MAX_BYTES,
};

static_assert(P25RLS_FRAME_MAX_BYTES <= FRAME_DATA_MAX_LEN, "P25RLS 最长帧超过框架暂存上限");

/*--- 命令字分派 ---
 *
 * 从任务里拆出来是为了能在 host 上直接喂帧测（套件 `test_p25rls`）：任务本体依赖
 * 静态队列，外部触发不了。命令字按**显式小端**从两个字节拼出来，不依赖
 * `-fshort-enums` 与 2 字节对齐（RLS 那边踩过一次，host 与固件行为会分叉）。 */
void app_p25rls_handle_frame(app_ccb_t *ccb, const uint8_t *frame, uint16_t len)
{
    if (len < P25RLS_FRAME_MIN_BYTES) return; /* 探针已拦过，这里只兜底 */

    const app_p25rls_frame_t *p25_frame = (const app_p25rls_frame_t *)frame;
    const uint16_t wire = (uint16_t)((uint16_t)p25_frame->cmd[0] |
                                     ((uint16_t)p25_frame->cmd[1] << 8));

    /* DATA 域 = 整帧 − 帧头(6) − BCC(1) − 帧尾(2) */
    const uint16_t payload_len = (uint16_t)(len - sizeof(app_p25rls_frame_t) - 3U);

    for (uint8_t i = 0; i < P25RLS_CMD_CNT; i++) {
        if ((uint16_t)g_p25rls_cmd_table[i].cmd == wire) {
            g_p25rls_cmd_table[i].handler_fn(ccb, p25_frame->data_bcc_tail, payload_len);
            return;
        }
    }

    printf("[p25rls] 未知命令字 0x%04X，整帧丢弃\n", (unsigned)wire);
}

/*--- 帧任务 ---*/
void app_p25rls_task(void *argument)
{
    (void)argument;

    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)s_p25rls_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_p25rls_msg_queue, msg, NULL, osWaitForever)) continue;

        app_p25rls_handle_frame(msg->ccb, msg->data, msg->data_len);
    }
}

/* ---- 帧探测 ----
 *
 * 只判"是不是本协议的一帧完整帧"：帧头 → 长度域上下界 → 整帧到齐 → 帧尾。
 * 帧头只拷 6 字节：伪帧时框架逐字节重跳，若按整帧拷就是 O(n²) 的 memcpy
 * （RLS 那边的实测结论，会把分发任务拖住几十毫秒）。 */
app_pcb_probe_state_t app_p25rls_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                             const app_ccb_src_t *src, uint8_t *scratch,
                                             uint16_t scratch_size, uint32_t *total_len,
                                             uint8_t *aux)
{
    (void)ccb;
    (void)src;
    (void)aux; /* 本协议不向协议任务传分类 */

    const ring_buffer_t *buff = self->rb;

    if (rb_avail(buff, nullptr) < sizeof(app_p25rls_frame_t)) return APP_PCB_PROBE_STATE_WAIT;

    const uint16_t head_bytes = (uint16_t)sizeof(app_p25rls_frame_t);
    const uint16_t head_cap   = (scratch_size < head_bytes) ? scratch_size : head_bytes;
    if (rb_peek(buff, 0, scratch, head_cap, nullptr) < head_bytes) return APP_PCB_PROBE_STATE_SKIP;

    const app_p25rls_frame_t *frame = (const app_p25rls_frame_t *)scratch;

    if ((P25RLS_HEAD_BYTE0 != frame->head[0]) || (P25RLS_HEAD_BYTE1 != frame->head[1]))
        return APP_PCB_PROBE_STATE_FAKE;

    const uint16_t total =
        (uint16_t)(((uint16_t)frame->length[0] << 8) | (uint16_t)frame->length[1]);

    /* 长度域是帧内取来的 16 位值，**必须**在拿它当偏移/字节数之前夹住 */
    if ((total < P25RLS_FRAME_MIN_BYTES) || (total > P25RLS_FRAME_MAX_BYTES))
        return APP_PCB_PROBE_STATE_FAKE;

    /* 整帧到齐之前不得读帧尾：否则读到的是后续字节，比对结果无意义 */
    if (rb_avail(buff, nullptr) < total) return APP_PCB_PROBE_STATE_WAIT;

    uint8_t tail0 = 0;
    uint8_t tail1 = 0;
    if (!rb_peekc(buff, (uint16_t)(total - 2U), &tail0, nullptr)) return APP_PCB_PROBE_STATE_SKIP;
    if (!rb_peekc(buff, (uint16_t)(total - 1U), &tail1, nullptr)) return APP_PCB_PROBE_STATE_SKIP;
    if ((P25RLS_TAIL_BYTE0 != tail0) || (P25RLS_TAIL_BYTE1 != tail1))
        return APP_PCB_PROBE_STATE_FAKE;

    *total_len = total;
    return APP_PCB_PROBE_STATE_READY;
}

/* ---- 公开访问器 ---- */
app_pcb_t *app_p25rls_pcb(void)
{
    return &s_p25rls_pcb;
}

/* ---- 协议自注册 ----
 *
 * 只绑 UDP（10011，回包走本帧来源的 IP:端口）。用 sw_app(3)：本协议不读配置，
 * 与 GZZH 同层；任务在通道任务之前建好即可。 */
[[maybe_unused]] static void _p25rls_module_init(void)
{
    rb_init(&s_p25rls_rb, "p25rls");

    s_p25rls_msg_queue = osMessageQueueNew(2, P25RLS_MSG_BYTES, &s_p25rls_queue_attr);
    s_p25rls_pcb.queue = s_p25rls_msg_queue;

    app_dispatch_bind(&s_p25rls_pcb, app_udp_ccb());

    s_p25rls_task_handle = pl_task_new(app_p25rls_task, nullptr, &s_p25rls_task_attr);
}
sw_app_initcall(_p25rls_module_init);
