/**
 * @file    app_gzzh.c
 * @brief   GZZH 协议主实现：帧探测、消息任务与命令分派
 *
 * 资源分配按"最长帧 = 框架暂存上限"给：本协议不限制文本长度，1472 字节的
 * UDP 报文一帧装得下，所以环形缓冲区按 CDSTD 的口径取 2 × 最长帧。
 */

#include "app_gzzh.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "initcall.h"

#include "app_gzzh_cmd.h"
#include "app_udp.h"
#include "pl_net.h"
#include "pl_mem.h"
#include "pl_task.h"

/* ---- 引导串（协议常量；命令层回帧也用），定义见头文件声明 ---- */
const uint8_t g_gzzh_head[GZZH_HEAD_BYTES] = {0x54, 0x43, 0x4C, 0x59};

#if GZZH_TRACE
/** @brief 联调期探针诊断的已打印行数（限次：伪帧会逐字节重跳，不限次会刷爆 RTT） */
static uint8_t s_gzzh_probe_diag;
#endif

/* ---- 队列与消息缓冲 ----
 * 元素 = 帧消息头 + 最长帧；两块在 CCMRAM（与 RLS/CDSTD 同一取值口径）。 */
#define GZZH_MSG_BYTES (sizeof(app_dispatch_msg_t) + GZZH_FRAME_MAX_BYTES)

static StaticQueue_t s_gzzh_queue_cb;
static uint8_t s_gzzh_queue_buf[2 * GZZH_MSG_BYTES] PL_CCMRAM;
static const osMessageQueueAttr_t s_gzzh_queue_attr = {
    .name    = "proto_gzzh_queue",
    .cb_mem  = &s_gzzh_queue_cb,
    .cb_size = sizeof(s_gzzh_queue_cb),
    .mq_mem  = s_gzzh_queue_buf,
    .mq_size = sizeof(s_gzzh_queue_buf),
};

/** @brief 任务取帧用的接收缓冲（比队列元素多一份，避免在任务栈上摊 1.4KB） */
static uint8_t s_gzzh_msg_buf[GZZH_MSG_BYTES];

static osMessageQueueId_t s_gzzh_msg_queue;
static osThreadId_t s_gzzh_task_handle;
static const osThreadAttr_t s_gzzh_task_attr = {
    .name       = "app_gzzh_task",
    .stack_size = 384 * 4, /* 命令层要跑字库渲染（app_render 的排版缓冲在它自己的静态区） */
    .priority   = (osPriority_t)osPriorityNormal,
};

/* ---- 协议控制块 ----
 * 环形缓冲区容量：`max(2 × 最长帧 1440, 单次最大写入 2048 + 1)` —— 后者是 DMA/UDP
 * 一次派发就能写进来的上限。**必须严格大于单次最大写入**：ring buffer 用保留一个
 * 空槽区分满/空（rb_space = size - avail - 1），取相等值时满的那一次会静默截掉末字节。 */
RB_DEFINE_ATTR(s_gzzh_rb, 2944, PL_CCMRAM);

static const app_pcb_ops_t s_gzzh_ops = {.probe = app_gzzh_probe_frame};

static app_pcb_t s_gzzh_pcb = {
    .name        = "gzzh",
    .ops         = &s_gzzh_ops,
    .rb          = &s_gzzh_rb,
    .payload_max = GZZH_FRAME_MAX_BYTES,
};

static_assert(GZZH_FRAME_MAX_BYTES <= FRAME_DATA_MAX_LEN, "GZZH 最长帧超过框架暂存上限");
static_assert(GZZH_FRAME_MIN_BYTES == sizeof(app_gzzh_frame_t) + 1U, "最短帧口径被改动了");

/*--- 命令字分派 ---
 *
 * 从任务里拆出来是为了能在 host 上直接喂帧测（套件 `test_gzzh`）：任务本体依赖静态
 * 队列，外部触发不了，而"命令字 → 处理函数"是唯一的分派路径，错一处就是"命令 A 干了
 * 命令 B 的事"，必须能测。
 *
 * 命令字按**显式小端**从四个字节拼出来，不用 `*(uint32_t *)frame->cmd`：那种写法依赖
 * 对齐与字节序，host 套件与固件都成立才行。 */
void app_gzzh_handle_frame(app_ccb_t *ccb, const uint8_t *frame, uint16_t len)
{
    if (len < GZZH_FRAME_MIN_BYTES) return; /* 探针已拦过，这里只兜底 */

    const app_gzzh_frame_t *gzzh_frame = (const app_gzzh_frame_t *)frame;
    const uint32_t wire = (uint32_t)gzzh_frame->cmd[0] | ((uint32_t)gzzh_frame->cmd[1] << 8) |
                          ((uint32_t)gzzh_frame->cmd[2] << 16) |
                          ((uint32_t)gzzh_frame->cmd[3] << 24);

    /* 载荷 = 整帧 − 引导段 − 结束符 */
    const uint16_t payload_len = (uint16_t)(len - sizeof(app_gzzh_frame_t) - 1U);

    GZZH_TRACE_PRINTF("[gzzh] RX ch=%s total=%u payload=%u cmd=0x%02lX\n", ccb->name,
                      (unsigned)len, (unsigned)payload_len, (unsigned long)wire);

    /* 命令字按**整 4 字节**比对（参考实现同口径）：表里全是 0x00~0xFF 的值，
       所以"高 3 字节必须为 0"由这次比较自然保证，不必另设守卫。 */
    for (uint8_t i = 0; i < GZZH_CMD_CNT; i++) {
        if ((uint32_t)g_gzzh_cmd_table[i].cmd == wire) {
            g_gzzh_cmd_table[i].handler_fn(ccb, gzzh_frame->payload, payload_len);
            return;
        }
    }

    printf("[gzzh] 未知命令字 0x%02lX，整帧丢弃\n", (unsigned long)wire);
}

/*--- 帧任务 ---*/
void app_gzzh_task(void *argument)
{
    (void)argument;

    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)s_gzzh_msg_buf;

    for (;;) {
        if (osOK != osMessageQueueGet(s_gzzh_msg_queue, msg, NULL, osWaitForever)) continue;

        app_gzzh_handle_frame(msg->ccb, msg->data, msg->data_len);
    }
}

/* ---- 帧探测 ----
 *
 * 只判"是不是本协议的一帧完整帧"：引导串 → 长度域上下界 → 整帧到齐 → 结束符。
 * 末尾那个"滚动的字节只拷引导段"的做法抄自 RLS：伪帧时框架逐字节重跳，
 * 若每次都按整帧拷，逐字节重跳就是 O(n²) 的 memcpy，会把分发任务拖住。 */
app_pcb_probe_state_t app_gzzh_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                           const app_ccb_src_t *src, uint8_t *scratch,
                                           uint16_t scratch_size, uint32_t *total_len, uint8_t *aux)
{
    (void)ccb;
    (void)src;
    (void)aux; /* 本协议不向协议任务传分类 */

    const ring_buffer_t *buff = self->rb;

    if (rb_avail(buff, nullptr) < sizeof(app_gzzh_frame_t)) return APP_PCB_PROBE_STATE_WAIT;

    const uint16_t head_bytes = (uint16_t)sizeof(app_gzzh_frame_t);
    const uint16_t head_cap   = (scratch_size < head_bytes) ? scratch_size : head_bytes;
    if (rb_peek(buff, 0, scratch, head_cap, nullptr) < head_bytes) return APP_PCB_PROBE_STATE_SKIP;

    const app_gzzh_frame_t *frame = (const app_gzzh_frame_t *)scratch;

    if (memcmp(g_gzzh_head, frame->head, sizeof(g_gzzh_head))) return APP_PCB_PROBE_STATE_FAKE;

    const uint16_t total =
        (uint16_t)((uint16_t)frame->length[0] | ((uint16_t)frame->length[1] << 8));

    /* 长度域是帧内取来的 16 位值，**必须**在拿它当偏移/字节数之前夹住：
       取 0 会让 `total - 1` 回绕到帧前，取 65535 会读到 64KB 之外。 */
    if ((total < GZZH_FRAME_MIN_BYTES) || (total > GZZH_FRAME_MAX_BYTES)) {
#if GZZH_TRACE
        /* 联调限次：上位机的"长度域"口径若不对，这一行是唯一线索 */
        if (s_gzzh_probe_diag < 8U) {
            s_gzzh_probe_diag++;
            printf("[gzzh] 帧头命中但长度域=%u 越界（合法 %u..%u），按伪帧跳过\n", (unsigned)total,
                   (unsigned)GZZH_FRAME_MIN_BYTES, (unsigned)GZZH_FRAME_MAX_BYTES);
        }
#endif
        return APP_PCB_PROBE_STATE_FAKE;
    }

    /* 整帧到齐之前不得读结束符：否则读到的是后续字节，比对结果无意义 */
    if (rb_avail(buff, nullptr) < total) return APP_PCB_PROBE_STATE_WAIT;

    /* 只窥视最后一个字节，不必把整帧拷进暂存区（READY 后框架自己 rb_read） */
    uint8_t tail = 0xFF;
    if (!rb_peekc(buff, (uint16_t)(total - 1U), &tail, nullptr)) return APP_PCB_PROBE_STATE_SKIP;
    if (GZZH_TAIL_BYTE != tail) {
#if GZZH_TRACE
        if (s_gzzh_probe_diag < 8U) {
            s_gzzh_probe_diag++;
            printf("[gzzh] 帧头命中、长度=%u，但结束符=0x%02X（应为 0x00）\n", (unsigned)total,
                   (unsigned)tail);
        }
#endif
        return APP_PCB_PROBE_STATE_FAKE;
    }

    *total_len = total;
    return APP_PCB_PROBE_STATE_READY;
}

/* ---- 公开访问器 ---- */
app_pcb_t *app_gzzh_pcb(void)
{
    return &s_gzzh_pcb;
}

/* ---- 协议自注册 ----
 *
 * 共享层只绑 UDP（现场 RJ45 就是这一路）；RS232-1 是板级事实，由 board 侧取
 * `app_gzzh_pcb()` 去绑（见 boards/3833024/Application/Src/app_rs232.c）。
 *
 * 用 sw_app(3)：本协议不读配置，无需排在 sw_post(4) 的"读配置"之后，
 * 但要早于板级 sw_post(4) 里那次 `app_gzzh_pcb()` 绑定 —— 同层次序不可依赖，
 * 跨层才确定（见 docs/架构说明.md §2）。 */
[[maybe_unused]] static void _gzzh_module_init(void)
{
    /* 联调：把**当前生效**的网络打出来（上位机要对着它发）。
       注意本函数在 sw_app(3)，与"读 IAP 记录并应用网络"同层 —— 两者先后由链接顺序
       决定，所以这里打的是"此刻"的值；若记录之后又把地址改了，紧接着还会有一条
       `[iap_cfg] 上电应用记录里的网络…`。 */
    uint8_t net_ip[4], net_mask[4], net_gw[4];
    pl_net_get_ip(net_ip, net_mask, net_gw);
    /* 这行**常开**（不是联调日志）：上电时把"本机此刻的地址"打出来，
       现场对地址最省事。注意它执行在 sw_app(3)，读 IAP 记录并应用网络的
       另一条 initcall 可能排在它之后 —— 若记录里的地址与默认不同，
       紧跟其后还会出现一条 `[iap_cfg] 上电应用记录里的网络…`。 */
    printf("[gzzh] 网络：IP %u.%u.%u.%u  掩码 %u.%u.%u.%u  网关 %u.%u.%u.%u\n",
                      (unsigned)net_ip[0], (unsigned)net_ip[1], (unsigned)net_ip[2],
                      (unsigned)net_ip[3], (unsigned)net_mask[0], (unsigned)net_mask[1],
                      (unsigned)net_mask[2], (unsigned)net_mask[3], (unsigned)net_gw[0],
                      (unsigned)net_gw[1], (unsigned)net_gw[2], (unsigned)net_gw[3]);

    rb_init(&s_gzzh_rb, "gzzh");

    s_gzzh_msg_queue = osMessageQueueNew(2, GZZH_MSG_BYTES, &s_gzzh_queue_attr);
    s_gzzh_pcb.queue = s_gzzh_msg_queue;

    app_dispatch_bind(&s_gzzh_pcb, app_udp_ccb());

    s_gzzh_task_handle = pl_task_new(app_gzzh_task, nullptr, &s_gzzh_task_attr);
}
sw_app_initcall(_gzzh_module_init);
