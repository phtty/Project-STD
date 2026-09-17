/**
 * @file    app_iap.c
 * @brief   IAP 固件升级协议处理
 *
 * 帧格式: 0x5A5A5A5A (4B) | seq (4B) | cmd (4B) | len (4B) | data | CRC32 (4B)
 * 承载于 RS485 + UDP，配置存储于 Flash 0x08004000。
 */

#include "app_iap.h"
#include "FreeRTOS.h"
#include "initcall.h"
#include "pl_crc.h"
#include "app_rs485.h"
#include "app_rs232.h"
#include "app_udp.h"
#include "app_iap_cfg.h"
#include "app_iap_cmd.h"
#include "pl_task.h"
#include "pl_mem.h"

/* ---- proto_iap_queue 静态分配 ---- */
#define IAP_PAYLOAD_MAX (1044U) /* FRAME_MAX_LEN * 4 */
#define IAP_MSG_SIZE (sizeof(frame_msg_t) + IAP_PAYLOAD_MAX)

static StaticQueue_t s_iap_queue_cb;
static uint8_t s_iap_queue_buf[2 * IAP_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_iap_queue_attr = {
    .name    = "proto_iap_queue",
    .cb_mem  = &s_iap_queue_cb,
    .cb_size = sizeof(s_iap_queue_cb),
    .mq_mem  = s_iap_queue_buf,
    .mq_size = sizeof(s_iap_queue_buf),
};

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ----
 * RB 容量必须**严格大于**传输层单次最大写入：ring buffer 保留一个空槽来区分满/空
 * （rb_space = size - avail - 1），所以装下 N 字节需要 size ≥ N+1。取相等的值会
 * 让 rb_write 静默截掉尾巴那一字节，帧尾被切 → CRC 失败 → 整帧丢。
 * 本协议承载 RS485/RS232（DMA 单次可达 2048）与 UDP，故取 2112 = 2048 + 余量。 */
RB_DEFINE_ATTR(s_iap_rb, 2112, PL_CCMRAM); /**< max(2 × 最长帧 1044, 单次最大写入 2048 + 1) */

static const pcb_ops_t s_iap_ops = {.probe = iap_probe_frame};

static pcb_t s_iap_pcb = {
    .name        = "iap",
    .ops         = &s_iap_ops,
    .rb          = &s_iap_rb,
    .payload_max = IAP_PAYLOAD_MAX,
};

static_assert(IAP_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "IAP 最长帧超过框架暂存上限");

/* ---- 协议模块自注册 ---- */
[[maybe_unused]] static void iap_module_init(void)
{
    rb_init(&s_iap_rb, "iap");

    /* 队列在 initcall 内建好：通道任务可能早于协议任务首次运行就投递帧，
       晚建会留下"向空队列投递"的窗口 */
    g_iap_msg_queue  = osMessageQueueNew(2, IAP_MSG_SIZE, &s_iap_queue_attr);
    s_iap_pcb.queue = g_iap_msg_queue;

    /* 绑定协议承载的通道。RS232 两路各是一个独立物理端点，与 RS485 同等对待。 */
    app_proto_bind(&s_iap_pcb, app_rs485_ccb());
    app_proto_bind(&s_iap_pcb, app_rs232_0_ccb());
    app_proto_bind(&s_iap_pcb, app_rs232_1_ccb());
    app_proto_bind(&s_iap_pcb, app_udp_ccb());

    /* 创建协议处理任务 */
    g_iap_task_handle = pl_task_new(iap_handle_task, nullptr, &iap_task_attr);
}
/* sw_post(4)：让"读配置"排在"加载配置"之后（cfg 调度器在 sw_app(3) 执行加载遍）。
   同层 initcall 的相对次序 = 链接顺序 = 构建清单文件次序，移动源文件即改变，
   不能用同层顺序表达依赖。 */
sw_post_initcall(iap_module_init);

const uint8_t frame_len[] = {0, 0, 4, 0, 1, 0, 0, 0};

osMessageQueueId_t g_iap_msg_queue;
osThreadId_t g_iap_task_handle;
const osThreadAttr_t iap_task_attr = {
    .name       = "iap_handle_task",
    .stack_size = 384 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ================================================================
 *  任务实现
 * ================================================================ */

/** @brief IAP 协议处理任务：阻塞等待帧队列 → 按 cmd 字段查表分派到命令处理函数 */
void iap_handle_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[IAP_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(g_iap_msg_queue, msg, NULL, osWaitForever))
            continue;

        iap_frame_t *frame_data = (iap_frame_t *)msg->data;

        /* 命令码由探针在投递当下从帧头解出并经 aux 带来，此处不必再解析一遍。
         *
         * **必须做范围检查**：cmd 是帧内的 & 0xFF 值（0~255），而命令表只有
         * IAP_CMD_COUNT 项。少了这一句就是拿帧里的值去索引函数指针表——
         * 帧过了 CRC32 就能构造，等于给出一条可控的越界函数调用。 */
        uint8_t cmd = msg->aux;
        if (cmd >= IAP_CMD_COUNT) continue;

        g_iap_cmd_table[cmd](msg->ccb, frame_data);
    }
}

/**
 * @brief   IAP 帧探测函数
 *
 * 检测 0x5A5A5A5A 帧头 → 校验 len 合法性 (<=256) → CRC32 验证 → 返回完整帧长度
 * @retval PCB_PROBE_READY  帧就绪
 * @retval PCB_PROBE_WAIT   数据不足
 * @retval PCB_PROBE_FAKE   伪帧头，跳过 1 字节重试
 * @retval PCB_PROBE_SKIP   帧长超出暂存区，无法校验，交由框架整帧跳过
 */
pcb_probe_sta_t iap_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux)
{
    (void)src; /* 本协议不区分来源 */
    (void)ccb;
    const ring_buffer_t *buff = self->rb;
    uint32_t available        = rb_avail(buff, nullptr);

    /* min size check */
    if (available < 4) return PCB_PROBE_WAIT;

    /* frame header check */
    uint32_t head = 0;
    rb_peek(buff, 0, (uint8_t *)&head, 4, nullptr);
    if (head != FRAME_HEAD)
        return PCB_PROBE_FAKE;

    /* payload len sanity check (protocol max 256) */
    uint32_t payload_len = 0;
    if (available >= (FRAME_LEN_OFFSET + 1) * 4) {
        rb_peek(buff, FRAME_LEN_OFFSET * 4, (uint8_t *)&payload_len, 4, nullptr);
        if (payload_len > 256)
            return PCB_PROBE_FAKE;
    } else {
        return PCB_PROBE_WAIT;
    }

    uint32_t full_bytes = (payload_len + FRAME_MIN_LEN) * 4;

    /* consecutive header check: if insufficient data but another 0x5A in range, skip */
    if (available < full_bytes) {
        for (uint32_t i = 1; i <= available - 4; i++) {
            uint32_t next_head = 0;
            rb_peek(buff, i, (uint8_t *)&next_head, 4, nullptr);
            if (next_head == FRAME_HEAD)
                return PCB_PROBE_FAKE;
        }
        return PCB_PROBE_WAIT;
    }

    /* 暂存区容量校验：payload_len 已被限制在 256，full_bytes 最大 1044 = 框架暂存上限。
       此处显式拦截是为了暂存区一旦被调小即在此拦下，而不是越界写。 */
    if (full_bytes > scratch_size) {
        *total_len = full_bytes;
        return PCB_PROBE_SKIP;
    }

    /* CRC32 validation（scratch 由框架提供且 4 字节对齐，可直接按 iap_frame_t 访问）*/
    rb_peek_capped(buff, 0, scratch, scratch_size, nullptr);
    iap_frame_t *ptemp = (iap_frame_t *)scratch;

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), scratch, (ptemp->len + 4) * 4);
    if (crc != ptemp->data_crc[ptemp->len])
        return PCB_PROBE_FAKE;

    *total_len = full_bytes;
    *aux       = ptemp->cmd & 0xFF;
    return PCB_PROBE_READY;
}
