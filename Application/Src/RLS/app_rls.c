#include "app_rls.h"
#include "FreeRTOS.h"
#include "initcall.h"

#include "bcc_utils.h"
#include "app_rls_cmd.h"
#include "app_rs485.h"
#include "pl_task.h"
#include "pl_mem.h"

/* ---- proto_rls_queue 静态分配 ---- */
#define RLS_PAYLOAD_MAX (530U) /* 帧头(6B) + bitmap(512B) + BCC(1B) + 尾(2B) + 余量 */
#define RLS_MSG_SIZE    (sizeof(app_dispatch_msg_t) + RLS_PAYLOAD_MAX)

static StaticQueue_t s_rls_queue_cb;
static uint8_t s_rls_queue_buf[2 * RLS_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_rls_queue_attr = {
    .name    = "proto_rls_queue",
    .cb_mem  = &s_rls_queue_cb,
    .cb_size = sizeof(s_rls_queue_cb),
    .mq_mem  = s_rls_queue_buf,
    .mq_size = sizeof(s_rls_queue_buf),
};

static const app_rls_cmd_type_t cmd_index_table[] = {
    APP_RLS_CMD_TYPE_TEST,
    APP_RLS_CMD_TYPE_DISPLAY,
    APP_RLS_CMD_TYPE_DISPLAY_SAVE,
};

osMessageQueueId_t g_rls_msg_queue;
osThreadId_t g_rls_task_handle;
const osThreadAttr_t g_rls_task_attr = {
    .name       = "app_rls_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/*--- 帧任务---*/
void app_rls_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[RLS_MSG_SIZE];
    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(g_rls_msg_queue, msg, NULL, osWaitForever))
            continue;

        app_rls_frame_t *rls_frame = (app_rls_frame_t *)(msg->data);

        /* 查表分派 */
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]); i++)
            if (cmd_index_table[i] == *(app_rls_cmd_type_t *)rls_frame->cmd)
                idx = i;

        if (idx < sizeof(cmd_index_table) / sizeof(cmd_index_table[0]))
            g_rls_cmd_table[idx](msg->ccb, rls_frame->data_bcc_tail);
    }
}

/* ---- 帧探测 ---- */
static const uint8_t rls_head[2] = {0xFF, 0xFE};
static const uint8_t rls_tail[2] = {0x0D, 0x0C};

/** 帧长下限：帧头(6B) + BCC(1B) + 尾(2B)，再短则长度域自相矛盾 */
#define RLS_FRAME_MIN (sizeof(app_rls_frame_t) + 3U)

app_pcb_probe_state_t app_rls_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux)
{
    (void)src;
    (void)ccb;
    (void)aux; /* 本协议不向协议任务传分类 */
    const ring_buffer_t *buff = self->rb;

    uint32_t avail = rb_avail(buff, nullptr);
    if (avail < sizeof(app_rls_frame_t) + 4)
        return APP_PCB_PROBE_STATE_WAIT;

    /* 窥视一律经 rb_peek_capped —— 按暂存区容量夹紧。此前 rb_peek(buff, 0, mem_pool,
       avail, nullptr) 配 525 字节的 mem_pool，而 rb 有 2048 字节，avail 超过 525
       即写穿栈数组。
       **而且这里只拷帧头那几个字节**：伪帧时框架会"跳 1 字节再探"，若每次都按
       `scratch_size` 拷一整帧，逐字节重跳就是 O(n²) 的 memcpy —— 级联那边实测：
       一条 1427 字节的杂物要拷 ~2MB，三个协议绑在同一条 485 上 ~6MB，把帧分发任务
       拖住 40ms+；而这段时间里后到的帧会把前一条**完整但还没轮到解析**的帧从协议
       缓冲里挤掉（"装不下就丢旧留新"）。判"是不是本协议的帧"只要这 6 个字节。 */
    const uint16_t head_cap = (scratch_size < (uint16_t)sizeof(app_rls_frame_t))
                                  ? scratch_size
                                  : (uint16_t)sizeof(app_rls_frame_t);
    if (rb_peek_capped(buff, 0, scratch, head_cap, nullptr) < sizeof(app_rls_frame_t))
        return APP_PCB_PROBE_STATE_SKIP;
    app_rls_frame_t *frame = (app_rls_frame_t *)scratch;

    if (memcmp(rls_head, frame->head, sizeof(rls_head)))
        return APP_PCB_PROBE_STATE_FAKE;

    uint16_t data_len = (uint16_t)((frame->length[1] & 0xFF) | ((frame->length[0] << 8) & 0xFF00));

    /* 长度域上界校验：data_len 是帧内取来的 16 位值，**必须**在拿它当偏移之前夹住。
       下面 (uint8_t *)frame + data_len - 2 是读操作，data_len 取 0 时回绕到帧前，
       取 65535 时读到 64KB 之外 —— 两处都在暂存区之外。 */
    if (data_len < RLS_FRAME_MIN || data_len > RLS_PAYLOAD_MAX)
        return APP_PCB_PROBE_STATE_FAKE;

    /* 整帧到齐之前不得读帧尾：否则读到的是后续字节，比对结果无意义 */
    if (avail < data_len)
        return APP_PCB_PROBE_STATE_WAIT;

    if (data_len > scratch_size) {
        *total_len = data_len; /* 暂存区装不下 → 无法校验，整帧丢弃 */
        return APP_PCB_PROBE_STATE_SKIP;
    }

    /* 到这里才需要整帧（尾标在帧尾）。拷的**正好是 data_len** —— 上面两条已经保证
       `data_len ≤ avail`（⑦）且 `data_len ≤ scratch_size`（⑧），所以既不越界、
       也不必像原先那样按 scratch_size 多拷一截无关数据。 */
    rb_peek_capped(buff, 0, scratch, data_len, nullptr);

    if (memcmp(rls_tail, (uint8_t *)frame + data_len - 2, sizeof(rls_tail)))
        return APP_PCB_PROBE_STATE_FAKE;

    // 上位机的bcc校验没有做
    // uint8_t frame_bcc = ((uint8_t *)frame)[data_len - 3];
    // uint8_t calc_bcc  = bcc_calc(frame->data_bcc_tail, data_len - sizeof(app_rls_frame_t) - 4);
    // if (frame_bcc != calc_bcc)
    //     return APP_PCB_PROBE_STATE_FAKE;

    *total_len = data_len;
    return APP_PCB_PROBE_STATE_READY;
}

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ----
 * 承载 RS485（DMA 单次可达 2048）。容量须**严格大于**单次最大写入：
 * ring buffer 保留一个空槽区分满/空（rb_space = size - avail - 1），取相等值时
 * 恰好满的那一次会静默截掉最后一个字节。 */
RB_DEFINE_ATTR(s_rls_rb, 2112, PL_CCMRAM); /**< max(2 × 最长帧 530, 单次最大写入 2048 + 1) */

static const app_pcb_ops_t s_rls_ops = {.probe = app_rls_probe_frame};

static app_pcb_t s_rls_pcb = {
    .name        = "rls",
    .ops         = &s_rls_ops,
    .rb          = &s_rls_rb,
    .payload_max = RLS_PAYLOAD_MAX,
};

static_assert(RLS_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "RLS 最长帧超过框架暂存上限");

/* ---- 协议自注册 ---- */
[[maybe_unused]] static void _rls_module_init(void)
{
    rb_init(&s_rls_rb, "rls");

    g_rls_msg_queue = osMessageQueueNew(2, RLS_MSG_SIZE, &s_rls_queue_attr);
    s_rls_pcb.queue = g_rls_msg_queue;

    app_dispatch_bind(&s_rls_pcb, app_rs485_ccb());

    g_rls_task_handle = pl_task_new(app_rls_task, nullptr, &g_rls_task_attr);
}
/* sw_post(4)：让"读配置"排在"加载配置"之后（cfg 调度器在 sw_app(3) 执行加载遍）。
   同层 initcall 的相对次序 = 链接顺序 = 构建清单文件次序，不能用它表达依赖。 */
sw_post_initcall(_rls_module_init);
