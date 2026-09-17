#include "app_rls.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "bcc_utils.h"
#include "app_rls_cmd.h"
#include "app_rs485.h"
#include "pl_task.h"

/* ---- proto_rls_queue 静态分配 ---- */
#define RLS_PAYLOAD_MAX (530U) /* 帧头(6B) + bitmap(512B) + BCC(1B) + 尾(2B) + 余量 */
#define RLS_MSG_SIZE    (sizeof(frame_msg_t) + RLS_PAYLOAD_MAX)

static StaticQueue_t s_rls_queue_cb;
static uint8_t s_rls_queue_buf[2 * RLS_MSG_SIZE];
static const osMessageQueueAttr_t s_rls_queue_attr = {
    .name    = "proto_rls_queue",
    .cb_mem  = &s_rls_queue_cb,
    .cb_size = sizeof(s_rls_queue_cb),
    .mq_mem  = s_rls_queue_buf,
    .mq_size = sizeof(s_rls_queue_buf),
};

static const rls_cmd_type_t cmd_index_table[] = {
    RLS_CMD_TEST,
    RLS_CMD_DISPLAY,
    RLS_CMD_DISPLAY_SAVE,
};

osMessageQueueId_t g_rls_msg_queue;
osThreadId_t g_rls_task_handle;
const osThreadAttr_t rls_task_attr = {
    .name       = "rls_handle_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/*--- 帧任务---*/
void rls_handle_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[RLS_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(g_rls_msg_queue, msg, NULL, osWaitForever))
            continue;

        rls_frame_t *rls_frame = (rls_frame_t *)(msg->data);

        /* 查表分派 */
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]); i++)
            if (cmd_index_table[i] == *(rls_cmd_type_t *)rls_frame->cmd)
                idx = i;

        if (idx < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]))
            g_rls_cmd_table[idx](msg->ccb, rls_frame->data_bcc_tail);
    }
}

/* ---- 帧探测 ---- */
static const uint8_t rls_head[2] = {0xFF, 0xFE};
static const uint8_t rls_tail[2] = {0x0D, 0x0C};

/** 帧长下限：帧头(6B) + BCC(1B) + 尾(2B)，再短则长度域自相矛盾 */
#define RLS_FRAME_MIN (sizeof(rls_frame_t) + 3U)

pcb_probe_sta_t rls_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux)
{
    (void)src;
    (void)ccb;
    (void)aux; /* 本协议不向协议任务传分类 */
    const ring_buffer_t *buff = self->rb;

    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < sizeof(rls_frame_t) + 4)
        return PCB_PROBE_WAIT;

    /* 窥视一律经 rb_peek_capped —— 按暂存区容量夹紧。此前 rb_peek(buff, 0, mem_pool,
       avail, nullptr) 配 525 字节的 mem_pool，而 rb 有 2048 字节，avail 超过 525
       即写穿栈数组。 */
    if (rb_peek_capped(buff, 0, scratch, scratch_size, nullptr) < sizeof(rls_frame_t))
        return PCB_PROBE_SKIP;
    rls_frame_t *frame = (rls_frame_t *)scratch;

    if (memcmp(rls_head, frame->head, sizeof(rls_head)))
        return PCB_PROBE_FAKE;

    uint16_t data_len = (uint16_t)((frame->length[1] & 0xFF) | ((frame->length[0] << 8) & 0xFF00));

    /* 长度域上界校验：data_len 是帧内取来的 16 位值，**必须**在拿它当偏移之前夹住。
       下面 (uint8_t *)frame + data_len - 2 是读操作，data_len 取 0 时回绕到帧前，
       取 65535 时读到 64KB 之外 —— 两处都在暂存区之外。 */
    if (data_len < RLS_FRAME_MIN || data_len > RLS_PAYLOAD_MAX)
        return PCB_PROBE_FAKE;

    /* 整帧到齐之前不得读帧尾：否则读到的是后续字节，比对结果无意义 */
    if (avail < data_len)
        return PCB_PROBE_WAIT;

    if (data_len > scratch_size) {
        *total_len = data_len; /* 暂存区装不下 → 无法校验，整帧丢弃 */
        return PCB_PROBE_SKIP;
    }

    rb_peek_capped(buff, 0, scratch, scratch_size, nullptr);

    if (memcmp(rls_tail, (uint8_t *)frame + data_len - 2, sizeof(rls_tail)))
        return PCB_PROBE_FAKE;

    // 上位机的bcc校验没有做
    // uint8_t frame_bcc = ((uint8_t *)frame)[data_len - 3];
    // uint8_t calc_bcc  = bcc_calcu(frame->data_bcc_tail, data_len - sizeof(rls_frame_t) - 4);
    // if (frame_bcc != calc_bcc)
    //     return PCB_PROBE_FAKE;

    *total_len = data_len;
    return PCB_PROBE_READY;
}

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ----
 * 承载 RS485（DMA 单次可达 2048）。容量须**严格大于**单次最大写入：
 * ring buffer 保留一个空槽区分满/空（rb_space = size - avail - 1），取相等值时
 * 恰好满的那一次会静默截掉最后一个字节。 */
RB_DEFINE(s_rls_rb, 2112); /**< max(2 × 最长帧 530, 单次最大写入 2048 + 1) */

static const pcb_ops_t s_rls_ops = {.probe = rls_probe_frame};

static pcb_t s_rls_pcb = {
    .name        = "rls",
    .ops         = &s_rls_ops,
    .rb          = &s_rls_rb,
    .payload_max = RLS_PAYLOAD_MAX,
};

static_assert(RLS_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "RLS 最长帧超过框架暂存上限");

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void rls_module_init(void)
{
    rb_init(&s_rls_rb, "rls");

    g_rls_msg_queue = osMessageQueueNew(2, RLS_MSG_SIZE, &s_rls_queue_attr);
    s_rls_pcb.queue = g_rls_msg_queue;

    app_proto_bind(&s_rls_pcb, app_rs485_ccb());

    g_rls_task_handle = pl_task_new(rls_handle_task, nullptr, &rls_task_attr);
}
/* sw_post(4)：让"读配置"排在"加载配置"之后（cfg 调度器在 sw_app(3) 执行加载遍）。
   同层 initcall 的相对次序 = 链接顺序 = 构建清单文件次序，不能用它表达依赖。 */
sw_post_initcall(rls_module_init);
