/**
 * @file    app_cascade.c
 * @brief   多控制卡级联同步显示协议 —— 探针、任务、枚举、整屏调光
 *
 * P2 阶段：**只做骨架与不依赖轮次机制的两条命令**（PING/PRESENT 枚举、
 * SET_BRIGHT 整屏调光）。位图传输、分片、应答、剔除留到 P3/P4。
 *
 * 骨架照 `Application/Src/RLS/app_rls.c`（收-only 最干净的那个）：
 * RB_DEFINE_ATTR → 静态 pcb_t → sw_post_initcall 里 rb_init / 建队列 / 绑通道 / 起任务。
 *
 * **绑 RS485**，且地址必须写在帧里 —— `ccb_dst_t` 只有 broadcast/topic 两个字段、
 * 没有地址，而 RS485 通道本来就忽略 dst（总线共享）。各从卡按帧里的 dst 过滤，
 * 与 LDI 用 lane_code 判"不属于本设备"→ SKIP 是同一做法。
 */

#include "app_cascade.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h" /* StaticQueue_t（静态队列控制块）*/
#include "app_rs485.h"
#include "app_screen.h"
#include "dev_display.h" /* PRESENT 要报本卡几何 */
#include "pl_crc.h"
#include "initcall.h"
#include "pl_mem.h"
#include "pl_task.h"

/* ---- 诊断输出 ----
 *
 * **两侧都要有**：只有一侧打日志时，"帧没到"与"帧到了但没处理"在现场分不开，
 * 而这两种情况的排查方向完全相反（查线 vs 查任务）。
 * 默认开；现场嫌吵可 -DCASC_DIAG=0 关掉，与 app_screen_status() 那条"上报是可选的"
 * 是两回事 —— 这是**本地**调试输出，不出设备。 */
#ifndef CASC_DIAG
#define CASC_DIAG (1)
#endif
#if CASC_DIAG
#define CASC_LOG(...) printf(__VA_ARGS__)
#else
#define CASC_LOG(...) ((void)0)
#endif

/* ---- 队列与缓冲区（容量取法见 app_iap.c / app_rls.c 的同名注释）---- */

/* **本协议真正会发出的最长帧**：分片帧 = 头 15 + CASC_FRAG_BYTES。不是 CASC_FRAME_MAX
 * （那是探针允许的最大帧长）—— 队列元素按后者算是 1052 字节，深 8 就要 8.2KB CCMRAM，
 * 而实际最大的一帧只有 527。把它同时设成 pcb.payload_max，框架就会把"超出本协议
 * 实际会发的长度"的帧判为违规并丢弃，队列元素因此有硬上界。 */
#define CASC_MSG_MAX (CASC_OVERHEAD + CASC_FRAG_BYTES)

_Static_assert(CASC_MSG_MAX >= CASC_OVERHEAD + sizeof(casc_sync_begin_t),
               "SYNC_BEGIN 比「最长帧」还长 —— 队列元素装不下");
_Static_assert(CASC_MSG_MAX >= CASC_OVERHEAD + sizeof(casc_ack_t), "ACK 装不下");

#define CASC_MSG_SIZE (sizeof(frame_msg_t) + CASC_MSG_MAX)

/* **深度 8 是照着一轮的连发算出来的，不是随手取的**：一轮 = BEGIN + N 片 + COMMIT，
   最多 5 帧（3 片）在 4ms 内连发完。而 ISR → rs485_task → frame_dispatch_task →
   本协议任务**四跳全是 Normal 优先级**，每跳最多等一个 tick，端到端几毫秒 ——
   深 2 的队列（P2 时期帧很稀疏，够用）在这中间是**静默丢帧**（框架的 Put 超时为 0），
   现场表现为"总缺中间那几片"，且两侧都看不出原因。

   实测就是这样：从卡回过一次 `缺片位 03`（只有第 2 片到了），而它本身工作正常。 */
#define CASC_QUEUE_DEPTH (8U)

static StaticQueue_t s_casc_queue_cb;
static uint8_t       s_casc_queue_buf[CASC_QUEUE_DEPTH * CASC_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_casc_queue_attr = {
    .name    = "proto_casc_queue",
    .cb_mem  = &s_casc_queue_cb,
    .cb_size = sizeof(s_casc_queue_cb),
    .mq_mem  = s_casc_queue_buf,
    .mq_size = sizeof(s_casc_queue_buf),
};

/* 必须**严格大于**传输层单次最大写入（RS485 的 DMA 缓冲 2048）；
   相等或更小会静默丢掉最后一字节 —— 见 app_iap.c 的说明。 */
RB_DEFINE_ATTR(s_casc_rb, 2112, PL_CCMRAM);

static osMessageQueueId_t s_casc_queue;

/* 上电时要先等从卡起来，故延后一点再发 PING */
#define CASC_BOOT_PING_DELAY_MS (3000U)
/* 亮度待下发的轮询周期 */
#define CASC_BRIGHT_POLL_MS (100U)

/* 等一条 ACK 的上限。40ms @115200 约合 460 字节的往返余量：一帧 ACK 只有 17 字节，
   剩下的都是对端的转发与任务调度延迟。 */
#define CASC_ACK_TIMEOUT_MS (40U)

/* 等 ACK 期间的轮询周期。只在一轮进行中才跑这么密，空闲时任务阻塞在队列上。 */
#define CASC_ACK_POLL_MS (1U)

/* **定向重传**次数上限。重传只补缺的那几片，所以多试一次很便宜；但也不能无限试 ——
   卡真的掉线时，每轮都卡在这儿会拖长整轮时间（其余卡与主卡本来不用等它）。 */
#define CASC_RETRY_MAX (2U)

/* 上电枚举：发 PING 等 PRESENT，**没等到就再发**，最多这么多轮、每轮隔这么久。
 *
 * **不能只发一次就下结论**：从卡可能比主卡晚就绪，或者那一帧正好赶上总线冲突。
 * 只发一次的话"链路其实没问题"会被报成"整个不通"—— 实测就撞上过：枚举说没人应答，
 * 紧接着的对齐轮次却全部成功，把人往接线/上电的方向引了半天。
 * （这也正是 P4 要做的"运行期重新枚举"的雏形。） */
#define CASC_PING_TRIES     (5U)
#define CASC_PING_RETRY_MS  (250U)

/* 枚举收尾后、第一轮对齐之前的间隔。**两者不能同刻** —— 半双工总线上一次只能有
   一个节点驱动，从卡回 PRESENT 时主卡若正在发分片，两边都成乱码（P3 的现场根因）。 */
#define CASC_BOOT_ALIGN_PING_GAP_MS (300U)

/* 上电对齐：枚举之后主动推一轮整屏，让所有卡一上电就是同一幅画面。
 *
 * **为什么不能只靠"画布有未落屏内容"**：上电时画布里的东西是从持久化恢复来的，
 * 它不算"新内容"（`_persist_restore` 明确把待落屏标志清掉了）—— 于是那一轮永远
 * 不会开，从卡会一直停在上电前它自己那幅旧画面上，直到有人下发一次。
 *
 * 首次可能撞上从卡还没起来，失败就隔 1 秒再来；试满仍不成则交给下一次真正的更新。 */
#define CASC_BOOT_ALIGN_TRIES    (5U)
#define CASC_BOOT_ALIGN_RETRY_MS (1000U)

/* 上电枚举的收卷时刻（0 = 不判定）与"是否见到过应答"。
 *
 * **为什么要专门报一句**：主卡收不到任何应答时，日志里只是"静悄悄地没有 PRESENT"，
 * 而那与"缓冲区被冲掉了""还没到点"分不开。而"从卡整个不答"与"答了但内容不对"
 * 是**完全相反**的两个排查方向（查线 vs 查协议），必须一刀切开。
 * 枚举与画布无关，所以这两个变量放在守卫之外。 */
static uint32_t s_enum_deadline;
static bool     s_enum_seen;

/* 上电枚举的剩余次数与下次发包时刻（0 = 枚举已收尾） */
static uint8_t  s_ping_left;
static uint32_t s_ping_next;

/* 上电对齐的剩余次数与下次尝试时刻（0 = 对齐已完成或尚未开始）。
   放在这儿（而不是 _round_run 旁边）是因为 _enum_finish 也要写它。
   跟着画布开关走：没有画布就没有"轮"，它自然也用不上。 */
#if BOARD_SCREEN_CANVAS
static uint8_t  s_align_left;
static uint32_t s_align_next;
#endif

/** @brief 枚举收尾：不再发 PING，等最后一帧 PRESENT 走完就开对齐轮 */
static void _enum_finish(uint32_t now)
{
    s_ping_left     = 0;
    s_enum_deadline = now + CASC_BOOT_ALIGN_PING_GAP_MS;
#if BOARD_SCREEN_CANVAS
    s_align_left = CASC_BOOT_ALIGN_TRIES;
    s_align_next = s_enum_deadline;
#endif
}

/* pcb 的字段在 _cascade_init 里填（探针定义在本文件后部，ops 要指向它） */
static pcb_t s_casc_pcb;

pcb_t *app_cascade_pcb(void)
{
    return &s_casc_pcb;
}

/* ================================================================
 *  探针
 * ================================================================ */

/** @brief 判断本帧是否该由本卡处理。
 *
 *  广播帧所有卡都收；其余只收自己地址的。**发送方自己也要按这条过滤** ——
 *  半双工总线上主卡可能收到自己发出去的回声（取决于 RE 是否与 DE 联动），
 *  而主卡地址是 0，单播给从卡的帧天然被它自己滤掉。
 *
 *  主卡的 PING/PRESENT 例外见各自的分派处：主卡发 PING 但不应答自己的 PING。 */
static bool _is_for_me(uint8_t dst)
{
    return dst == CASC_ADDR_BCAST || dst == app_screen_self_addr();
}

/** @brief 探测一帧
 *
 *  四态与边界处理的依据见 app_dispatch.h 的 probe 契约：
 *   · 调用者**已持 rb 锁**，故所有 rb API 传 nullptr 跳过二次加锁
 *   · 只窥视不消费
 *   · READY 必须写 *total_len；SKIP 也要写（框架按它跳过整帧）
 *   · 帧长超过 scratch_size 必须 SKIP，不得越界写
 *
 *  **校验顺序：长度域夹紧 → 等整帧 → CRC → 地址过滤。**
 *  CRC 放在地址过滤之前是刻意的：地址过滤会 SKIP 掉整个 len，若 len 被干扰破坏，
 *  就会连带吞掉后面一帧真帧；而 CRC 失败走 FAKE（只跳 1 字节重试），能在下一个
 *  真 SOF 处重新对上。代价是给别人的帧也要算一次 CRC（约 60µs @1KB），
 *  换回来的是可靠的再同步。 */
static pcb_probe_sta_t casc_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                        uint8_t *scratch, uint16_t scratch_size,
                                        uint32_t *total_len, uint8_t *aux)
{
    (void)ccb;
    (void)src; /* RS485 无来源概念 */

    uint16_t avail = rb_peek_capped(self->rb, 0, scratch, scratch_size, nullptr);
    if (avail < sizeof(casc_hdr_t)) return PCB_PROBE_WAIT;

    casc_hdr_t *h = (casc_hdr_t *)scratch;
    if (h->sof[0] != CASC_SOF0 || h->sof[1] != CASC_SOF1) return PCB_PROBE_FAKE;

    uint16_t len = casc_get_u16(h->len);

    /* 长度域先夹：合法级联帧永远在 [MIN, MAX] 内。越界说明这是伪同步，
       逐字节重跳能最快找回真帧；若返回 SKIP 会按伪长度把后面的真帧一起吞掉。 */
    if (len < CASC_FRAME_MIN || len > CASC_FRAME_MAX) return PCB_PROBE_FAKE;

    /* 整帧到齐前不碰帧尾/CRC —— 否则读到的是后续字节 */
    if (avail < len) return PCB_PROBE_WAIT;

    if (len > scratch_size) {
        *total_len = len;
        return PCB_PROBE_SKIP; /* 本设计下不可达（static_assert 保证），按契约保留 */
    }

    /* CRC32 覆盖 [2, len-4)：不含 SOF（恒定值不增加信息量）、不含 CRC 自身。
       走硬件单元（1KB 约 9µs；软件 CRC16 要 71µs）。
       注意区段起点 scratch+2 **不对齐** —— pl_crc32_calc 原先对非对齐输入只算前
       256 字节，正是为此先修了它（见该提交）。 */
    uint32_t crc_calc = pl_crc32_calc(pl_crc_get_handle(), scratch + 2, (size_t)(len - 6U));
    uint32_t crc_recv = casc_get_u32(scratch + len - 4U);
    if (crc_calc != crc_recv) return PCB_PROBE_FAKE;

    if (!_is_for_me(h->dst)) {
        *total_len = len;
        return PCB_PROBE_SKIP; /* 帧合法，只是不是给我这张卡的 */
    }

    *total_len = len;
    *aux       = CASC_TYPE_OF(h->ver_type);
    return PCB_PROBE_READY;
}

/* ================================================================
 *  发送
 * ================================================================ */

/** @brief 组一帧到 buf，返回整帧长度；0 表示载荷放不下
 *
 *  `idx`/`frag_n` 只有分片命令（SYNC_DATA）才非 0，其余一律 0 —— 但反正帧头里
 *  就有这两个字段，与其分两条组帧路径，不如让调用方显式给。 */
static uint16_t _build(uint8_t *buf, uint16_t buf_cap, uint8_t type, uint8_t dst, uint16_t seq,
                       uint8_t idx, uint8_t frag_n, const void *payload, uint16_t payload_len)
{
    uint16_t len = (uint16_t)(CASC_OVERHEAD + payload_len);
    if (len > buf_cap || len > CASC_FRAME_MAX) return 0;

    casc_hdr_t *h = (casc_hdr_t *)buf;
    h->sof[0]     = CASC_SOF0;
    h->sof[1]     = CASC_SOF1;
    h->ver_type   = (uint8_t)((CASC_PROTO_VER << 6) | CASC_TYPE_OF(type));
    h->dst        = dst;
    h->src        = app_screen_self_addr();
    casc_put_u16(h->seq, seq);
    h->idx    = idx;
    h->frag_n = frag_n;
    casc_put_u16(h->len, len);

    if (payload_len) memcpy(buf + sizeof(casc_hdr_t), payload, payload_len);

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), buf + 2, (size_t)(len - 6U));
    casc_put_u32(buf + len - 4U, crc);
    return len;
}

/** @brief 发一帧，**序号由调用方给**。
 *
 *  轮次类命令（BEGIN/DATA/COMMIT）与应答（ACK/NACK）必须用**本轮那个 seq**，
 *  不能用发送方自己的计数器：主卡靠 seq 认"这条 ACK 是答哪一轮的"，从卡回的 ACK
 *  若带自己的序号，主卡永远匹配不上，表现是"每张卡都超时"。
 *
 *  **缓冲必须 DMA 可达**（RS485 走 DMA 发送），所以用静态 SRAM 缓冲而不是栈上数组 ——
 *  栈在 SRAM 里其实也够得到，但要留一份给将来"发送期间缓冲必须一直有效"的余量，
 *  且静态缓冲让 ccb_send 的阻塞语义不依赖调用栈深度。 */
static int32_t _send_seq(uint8_t type, uint8_t dst, uint16_t seq, uint8_t idx, uint8_t frag_n,
                         const void *payload, uint16_t payload_len)
{
    static uint8_t s_tx[CASC_FRAME_MAX];

    uint16_t len = _build(s_tx, sizeof(s_tx), type, dst, seq, idx, frag_n, payload, payload_len);
    if (!len) return -1;

    const int32_t r = ccb_send(app_rs485_ccb(), s_tx, len);
    if (r < 0) {
        /* 通道没 UP / 平台层拒收。**只报前几次**：真出问题时每轮都会失败，
           不限量会把 RTT 冲干净，反而看不到别的。 */
        static uint8_t s_fail_logged;
        if (s_fail_logged < 4U) {
            s_fail_logged++;
            CASC_LOG("[casc] 发送**没出去**（%u 字节，类型 %02X）—— 通道未 UP 或平台层拒绝\n",
                     (unsigned)len, (unsigned)type);
        }
    }
    return (int32_t)len;
}

/** @brief 发一帧不需要轮次序号的（PING/PRESENT/SET_BRIGHT），序号取自增计数器 */
static int32_t _send(uint8_t type, uint8_t dst, const void *payload, uint16_t payload_len)
{
    static uint16_t s_seq;
    return _send_seq(type, dst, s_seq++, 0, 0, payload, payload_len);
}

int32_t app_cascade_ping(void)
{
    return _send(CASC_T_PING, CASC_ADDR_BCAST, nullptr, 0);
}

int32_t app_cascade_broadcast_bright(uint8_t level)
{
    casc_set_bright_t p = {.level = (uint8_t)(level > 7 ? 7 : level)};
    return _send(CASC_T_SET_BRIGHT, CASC_ADDR_BCAST, &p, sizeof(p));
}

/* ================================================================
 *  命令处理
 *
 *  P2 只三条：PING / PRESENT / SET_BRIGHT。分派按 `msg->aux` 查表 ——
 *  与 app_iap.c 同一做法，**且必须先做范围检查再索引**（那里写着不检查就等于
 *  给出一条可控的越界函数调用）。
 * ================================================================ */

static void _cmd_ping(frame_msg_t *msg)
{
    (void)msg;
    /* 主卡发 PING，从卡应答。主卡收到自己的 PING（或回声）时什么都不做 ——
       否则主卡会把自己当从卡应答一次，总线上出现两条 PRESENT。 */
    if (app_screen_is_master()) return;

    /* 按地址错开应答，避免多张从卡同时抢半双工总线。
       延时放在任务里（不在探针里）—— 探针在分发任务上下文中，堵它等于堵所有协议。 */
    osDelay((uint32_t)(app_screen_self_addr() - 1U) * 3U);

    dev_display_t *d = dev_display_get();
    casc_present_t p = {
        .addr      = app_screen_self_addr(),
        .bright    = app_screen_get_brightness(),
        .proto_ver = CASC_PROTO_VER,
    };
    uint16_t w = d ? d->screen_rows : 0;
    uint16_t h = d ? d->screen_cols : 0;
    casc_put_u16(p.w, w);
    casc_put_u16(p.h, h);

    (void)_send(CASC_T_PRESENT, CASC_ADDR_MASTER, &p, sizeof(p));
}

static void _cmd_present(frame_msg_t *msg)
{
    /* 主卡收：枚举结果先打出来。P4 建卡表时这里改成填表 + 校验几何。 */
    if (!app_screen_is_master()) return;
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_present_t))) return;

    const casc_present_t *p = (const casc_present_t *)(msg->data + sizeof(casc_hdr_t));

    s_enum_seen = true; /* 上电枚举据此判"有没有卡应答" */

    printf("[casc] PRESENT addr=%u %ux%u bright=%u ver=%u\n", (unsigned)p->addr,
           (unsigned)casc_get_u16(p->w), (unsigned)casc_get_u16(p->h), (unsigned)p->bright,
           (unsigned)p->proto_ver);
}

static void _cmd_set_bright(frame_msg_t *msg)
{
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_set_bright_t))) return;

    const casc_set_bright_t *p = (const casc_set_bright_t *)(msg->data + sizeof(casc_hdr_t));

    /* 广播的接收方一律套用（包括主卡自己）—— 幂等，且省掉"谁是发出者"的特判。
       主卡本机亮度在生成广播时已经设过，这里再设一次无害。 */
    app_screen_set_brightness(p->level);
}

/* ================================================================
 *  图传（从卡侧）—— 收分片、暂存、收到 COMMIT 且收齐才落屏
 *
 *  **先攒后换**，不边收边画：边收边落会出半幅画面（片上去了、下一片还没来），
 *  而半双工总线上这个窗口是毫秒级、肉眼可见。
 * ================================================================ */

static struct {
    uint8_t  stage[BOARD_CASCADE_BAND_MAX]; /**< 本轮位图暂存 */
    uint16_t seq;        /**< 本轮的轮次序号；**陈旧分片靠它丢** */
    uint16_t bmp_len;    /**< 本轮位图总字节数 */
    uint16_t frag_bytes; /**< 每片载荷字节数 */
    uint8_t  frag_n;     /**< 本轮分片数 */
    uint8_t  have_mask;  /**< 位 i = 第 i 片已收到 */
    uint8_t  color;      /**< 本卡颜色 */
    uint8_t  bright;     /**< 最近一次 BEGIN 断言的亮度（诊断用） */
    bool     active;     /**< 收到过本轮 BEGIN */
} s_rx;

static void _cmd_sync_begin(frame_msg_t *msg)
{
    /* 主卡不该收到发给从卡的 BEGIN（半双工回声、或两张卡地址配重时会）。
       不应答、不入暂存 —— 否则总线上会多出一个应答源，主卡把自己当从卡。 */
    if (app_screen_is_master()) return;
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_sync_begin_t))) return;

    const casc_hdr_t        *h = (const casc_hdr_t *)msg->data;
    const casc_sync_begin_t *p = (const casc_sync_begin_t *)(msg->data + sizeof(casc_hdr_t));

    const dev_display_t *d      = dev_display_get();
    const uint16_t       w      = casc_get_u16(p->w);
    const uint16_t       hh     = casc_get_u16(p->h);
    const uint16_t       bm_len = casc_get_u16(p->bmp_len);
    const uint16_t       frag_b = casc_get_u16(p->frag_bytes);

    /* 几何/参数不符 → 明确回 NACK，**不将就**。将就的后果是错位画面，而从卡自己
       不知道错位了（它会以为一切正常地把半幅图显示出去）。
       本工程的部署形态是"每张卡各带一整块屏"，所以矩形尺寸必须 == 本卡屏几何。 */
    if (!d || w != d->screen_rows || hh != d->screen_cols || bm_len == 0 ||
        bm_len > sizeof(s_rx.stage) || p->frag_n == 0 || p->frag_n > CASC_FRAG_MAX ||
        frag_b == 0 || (uint32_t)frag_b * p->frag_n < bm_len) {
        const casc_nack_t nack = {.err = CASC_NACK_GEOM};
        s_rx.active            = false;
        CASC_LOG("[casc·从] 拒绝 BEGIN：矩形 %ux%u 或分片参数不符（本卡屏 %ux%u，暂存 %u）\n",
                 (unsigned)w, (unsigned)hh, (unsigned)(d ? d->screen_rows : 0),
                 (unsigned)(d ? d->screen_cols : 0), (unsigned)sizeof(s_rx.stage));
        /* 回带**本轮序号**：主卡靠它认"这条 NACK 是答哪一轮的" */
        (void)_send_seq(CASC_T_NACK, CASC_ADDR_MASTER, casc_get_u16(h->seq), 0, 0, &nack,
                        sizeof(nack));
        return;
    }

    s_rx.seq        = casc_get_u16(h->seq);
    s_rx.bmp_len    = bm_len;
    s_rx.frag_bytes = frag_b;
    s_rx.frag_n     = p->frag_n;
    s_rx.have_mask  = 0;
    s_rx.color      = p->color;
    s_rx.bright     = p->bright;
    s_rx.active     = true;

    CASC_LOG("[casc·从] BEGIN seq=%u %ux%u@(%u,%u) 位图%u=%u片×%u bright=%u\n", (unsigned)s_rx.seq,
             (unsigned)w, (unsigned)hh, (unsigned)casc_get_u16(p->x), (unsigned)casc_get_u16(p->y),
             (unsigned)bm_len, (unsigned)p->frag_n, (unsigned)frag_b, (unsigned)p->bright);

    /* **每一轮都重新断言亮度**。SET_BRIGHT 是单次广播、没有重传，而本帧是每轮必发、
       丢一轮就自己自愈的一字节 —— 这是"从卡永久停在旧亮度上"的唯一防线。 */
    app_screen_set_brightness(p->bright);
}

static void _cmd_sync_data(frame_msg_t *msg)
{
    if (app_screen_is_master()) return;

    const casc_hdr_t *h   = (const casc_hdr_t *)msg->data;
    const uint16_t    seq = casc_get_u16(h->seq);

    /* **陈旧分片必须丢**：上一轮的迟到分片帧头完好、CRC 也是对的，探针照常放行 ——
       只能靠 seq 拦。放进去的后果是画面"一半旧内容一半新内容"，且从卡不知道自己错了。 */
    if (!s_rx.active || seq != s_rx.seq || h->frag_n != s_rx.frag_n) return;

    const uint8_t idx = h->idx;
    if (idx >= s_rx.frag_n) return;

    const uint16_t off = (uint16_t)((uint32_t)idx * s_rx.frag_bytes);
    if (off >= s_rx.bmp_len) return;

    const uint16_t n = (uint16_t)(msg->data_len - CASC_OVERHEAD);
    const uint16_t want =
        (uint16_t)(((uint32_t)s_rx.bmp_len - off > s_rx.frag_bytes)
                       ? s_rx.frag_bytes
                       : (s_rx.bmp_len - off));

    /* 长度必须**正好**是这一片该有的字节数。短一截的分片若不拒，会静默留下上一轮的
       旧字节，而 have_mask 记成"这片到了" —— 主卡便再也不会补发，错内容永久留在屏上。 */
    if (n != want) return;

    memcpy(&s_rx.stage[off], msg->data + sizeof(casc_hdr_t), n);
    s_rx.have_mask |= (uint8_t)(1U << idx);
}

static void _cmd_sync_commit(frame_msg_t *msg)
{
    if (app_screen_is_master()) return;

    const casc_hdr_t *h   = (const casc_hdr_t *)msg->data;
    casc_ack_t        ack = {.sta = CASC_ACK_OK, .miss_mask = 0};

    if (!s_rx.active || casc_get_u16(h->seq) != s_rx.seq) {
        /* 没收到 BEGIN：本轮的 DATA 也全被丢了，所以让主卡**整轮重来**，
           而不是只补几片（补片也没意义 —— 暂存是空的） */
        ack.sta = CASC_ACK_NOBEGIN;
    } else {
        const uint8_t full = (uint8_t)((1U << s_rx.frag_n) - 1U);
        ack.miss_mask      = (uint8_t)(full & ~s_rx.have_mask);
        if (ack.miss_mask) {
            ack.sta = CASC_ACK_MISS;
        } else {
            /* 收齐了才落屏。**幂等**：主卡没收到 ACK 会重发 COMMIT，重落同一份内容
               没有副作用 —— 所以这里**不清** active/have_mask。清了的话第二次 COMMIT
               要回 NOBEGIN，主卡会以为整轮白做、重发 3 片。下一轮的 BEGIN 会重置它们。 */
            app_screen_commit_bitmap(s_rx.stage, s_rx.bmp_len, s_rx.color);
        }
    }

    CASC_LOG("[casc·从] COMMIT seq=%u 收到片=%02X → sta=%u 缺=%02X\n",
             (unsigned)casc_get_u16(h->seq), (unsigned)s_rx.have_mask, (unsigned)ack.sta,
             (unsigned)ack.miss_mask);

    /* 回带**本轮序号**（不是自己的发送计数器）—— 主卡的等待循环按 (seq, src) 匹配 */
    (void)_send_seq(CASC_T_ACK, CASC_ADDR_MASTER, casc_get_u16(h->seq), 0, 0, &ack, sizeof(ack));
}

static void _cmd_sync_abort(frame_msg_t *msg)
{
    (void)msg;
    if (app_screen_is_master()) return;
    /* 主卡放弃本轮：清掉暂存，免得半份内容留在那儿被下一轮的 COMMIT 误用 */
    s_rx.active    = false;
    s_rx.have_mask = 0;
}

/* ================================================================
 *  图传（主卡侧）—— 结算用收到的应答
 *
 *  **收在这儿、由开轮的那段读**：两者跑在同一个任务里先后发生，不需要锁。
 *  等待循环按 (seq, src) 双重匹配 —— 总线是共享的，迟到的、别张卡的应答都会
 *  落进这里，只按 sta 判会把它们当成当前这张卡的答复。
 * ================================================================ */

#define CASC_STA_NACK (0xFFU) /**< 内部用：本帧是 NACK 而不是 ACK（不在 casc_ack_sta_t 里） */

static struct {
    uint16_t seq;       /**< 应答回带的轮次序号 */
    uint8_t  src;       /**< 谁答的（总线地址） */
    uint8_t  sta;       /**< casc_ack_sta_t，或 CASC_STA_NACK */
    uint8_t  miss_mask; /**< sta == CASC_ACK_MISS 时有效 */
    bool     valid;
} s_ack;

static void _cmd_ack(frame_msg_t *msg)
{
    if (!app_screen_is_master()) return; /* 从卡只发不收 */
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_ack_t))) return;

    const casc_hdr_t  *h = (const casc_hdr_t *)msg->data;
    const casc_ack_t  *a = (const casc_ack_t *)(msg->data + sizeof(casc_hdr_t));

    /* **收到就打**，不等匹配：只有这一行能把"字节根本没到"与"到了但没对上"分开，
       而这两者排查方向相反（查接收链路 vs 查匹配条件）。 */
    CASC_LOG("[casc·主] ← 收到 ACK seq=%u src=%u sta=%u 缺=%02X\n", (unsigned)casc_get_u16(h->seq),
             (unsigned)h->src, (unsigned)a->sta, (unsigned)a->miss_mask);

    s_ack.seq       = casc_get_u16(h->seq);
    s_ack.src       = h->src;
    s_ack.sta       = a->sta;
    s_ack.miss_mask = a->miss_mask;
    s_ack.valid     = true;
}

static void _cmd_nack(frame_msg_t *msg)
{
    if (!app_screen_is_master()) return;
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_nack_t))) return;

    const casc_hdr_t *h = (const casc_hdr_t *)msg->data;

    CASC_LOG("[casc·主] ← 收到 NACK seq=%u src=%u err=%u\n", (unsigned)casc_get_u16(h->seq),
             (unsigned)h->src, (unsigned)msg->data[sizeof(casc_hdr_t)]);

    s_ack.seq       = casc_get_u16(h->seq);
    s_ack.src       = h->src;
    s_ack.sta       = CASC_STA_NACK;
    s_ack.miss_mask = msg->data[sizeof(casc_hdr_t)]; /* err */
    s_ack.valid     = true;
}

typedef void (*casc_cmd_fn_t)(frame_msg_t *msg);

/* 按帧类型索引。0 项留空 = 未实现或不支持（SET_COLOR/SET_LAYOUT/BLANK 归后续期）。 */
static const casc_cmd_fn_t g_casc_cmd[CASC_TYPE_MASK + 1U] = {
    [CASC_T_SYNC_BEGIN]  = _cmd_sync_begin,
    [CASC_T_SYNC_DATA]   = _cmd_sync_data,
    [CASC_T_SYNC_COMMIT] = _cmd_sync_commit,
    [CASC_T_SYNC_ABORT]  = _cmd_sync_abort,
    [CASC_T_PING]        = _cmd_ping,
    [CASC_T_PRESENT]     = _cmd_present,
    [CASC_T_SET_BRIGHT]  = _cmd_set_bright,
    [CASC_T_ACK]         = _cmd_ack,
    [CASC_T_NACK]        = _cmd_nack,
};

/* ================================================================
 *  取帧与排空
 *
 *  **必须主动排空**：队列深度只有 2，而框架在队列满时**静默丢帧**
 *  （app_dispatch.c 的 osMessageQueuePut 超时为 0）。一轮图传是"每毫秒一帧"地
 *  连发，处理一条就回去睡的话从第 3 帧起全被丢掉 —— 现场表现是"总缺第 2、3 片"，
 *  且两侧都看不出原因。
 * ================================================================ */

/** @brief 取一条帧（最多等 timeout_ms）并按类型分派；返回是否有帧被处理 */
static bool _casc_pump(uint32_t timeout_ms)
{
    static uint8_t     msg_buf[CASC_MSG_SIZE] __attribute__((aligned(4)));
    frame_msg_t *const msg = (frame_msg_t *)msg_buf;

    if (osMessageQueueGet(s_casc_queue, msg, nullptr, timeout_ms) != osOK) return false;

    const uint8_t type = msg->aux;
    if (type <= CASC_TYPE_MASK && g_casc_cmd[type]) g_casc_cmd[type](msg);
    return true;
}

static void _casc_drain(void)
{
    while (_casc_pump(0)) { }
}

#if BOARD_SCREEN_CANVAS

/** @brief 等 `from` 那张卡对本轮 `seq` 的应答；返回 false = 超时
 *
 *  **等待期间持续分派**：队列深度只有 2，不排空的话后到的 ACK 会被丢掉，而
 *  "ACK 丢了"与"卡没应答"在现场分不开 —— 两者都表现为这张卡超时。
 *  按 (seq, src) 双重匹配：总线上会有别张卡的应答与上一轮的迟到应答。 */
static bool _wait_ack(uint8_t from, uint16_t seq, uint32_t deadline)
{
    for (;;) {
        _casc_drain();
        if (s_ack.valid && s_ack.seq == seq && s_ack.src == from) return true;
        if ((int32_t)(osKernelGetTickCount() - deadline) >= 0) return false;
        osDelay(CASC_ACK_POLL_MS);
    }
}

/* ================================================================
 *  开轮（主卡侧）
 *
 *  一轮 = 把**整屏**的新内容分发给每张从卡（各发它那一块），全部结算完后
 *  主卡自己也换帧。刷新需求是指令式的（几秒~几分钟一次），所以一轮 ~130ms/卡
 *  完全够用 —— 这也是不做差分模式的前提。
 * ================================================================ */

/* 主卡下发用的矩形缓冲。**与 s_rx.stage 分开**：一张卡要么是主卡要么是从卡，
   理论上能复用一块内存，但把两种角色的缓冲分开不容易出"串味"的错，代价只有 1.4KB。 */
static uint8_t s_band[BOARD_CASCADE_BAND_MAX];

static uint16_t s_round_seq;


/** @brief 把 `mask` 位选中的分片发出去（每片 CASC_FRAG_BYTES 字节）
 *
 *  **每片之间 osDelay(1)**：对端的 UART 靠**空闲中断**分帧（pl_uart.c 的
 *  HAL_UART_Receive_DMA 是非循环模式），帧连着发、线路一直不空闲，对端就永远
 *  等不到 IDLE，DMA 缓冲填满后接收直接停摆 —— 表现是"整轮一片都没收到"。
 *  1ms @115200 ≈ 11.5 个字节时间，足够 IDLE 判定；代价约 4%。 */
static void _send_frags(const screen_card_t *c, uint16_t seq, uint16_t bmp_len, uint8_t frag_n,
                        uint8_t mask)
{
    for (uint8_t k = 0; k < frag_n; k++) {
        if (!(mask & (uint8_t)(1U << k))) continue;

        const uint16_t off   = (uint16_t)(k * CASC_FRAG_BYTES);
        const uint32_t left  = (uint32_t)bmp_len - off; /* 无符号，避免与 CASC_FRAG_BYTES 的符号比较 */
        const uint16_t n     = (uint16_t)((left > CASC_FRAG_BYTES) ? CASC_FRAG_BYTES : left);

        (void)_send_seq(CASC_T_SYNC_DATA, c->addr, seq, k, frag_n, &s_band[off], n);
        osDelay(1);
    }
}

/** @brief 单张从卡的一轮：BEGIN → 分片 → 结算（含定向重传）
 *
 *  **逐卡走完再下一张**（而不是"以分片为主序、每片发给所有卡"）：一张卡的矩形要
 *  抽进 s_band，逐卡走完只需一块缓冲；分片为主序则每发一片都要重抽一次。代价是
 *  卡与卡之间多一次应答往返，3 卡约 120ms，对指令式刷新无所谓。 */
static bool _round_one_card(uint8_t idx, uint16_t seq, uint8_t bright)
{
    const screen_card_t *c       = app_screen_card(idx);
    const uint16_t       bmp_len = app_screen_card_bm_len(idx);
    if (!c || !bmp_len || bmp_len > sizeof(s_band)) return false;

    if (!app_screen_extract(idx, s_band, sizeof(s_band))) return false;

    const uint8_t frag_n = (uint8_t)((bmp_len + CASC_FRAG_BYTES - 1U) / CASC_FRAG_BYTES);
    if (frag_n == 0 || frag_n > CASC_FRAG_MAX) {
        printf("[casc] 卡 %u 的位图 %u 字节需 %u 片，超出 CASC_FRAG_MAX=%u，本轮跳过\n",
               (unsigned)c->addr, (unsigned)bmp_len, (unsigned)frag_n, (unsigned)CASC_FRAG_MAX);
        return false;
    }
    const uint8_t full = (uint8_t)((1U << frag_n) - 1U);

    casc_sync_begin_t p;
    casc_put_u16(p.x, c->x);
    casc_put_u16(p.y, c->y);
    casc_put_u16(p.w, c->w);
    casc_put_u16(p.h, c->h);
    casc_put_u16(p.bmp_len, bmp_len);
    casc_put_u16(p.frag_bytes, CASC_FRAG_BYTES);
    p.frag_n = frag_n;
    p.bright = bright;
    p.color  = c->color;

    s_ack.valid = false;
    (void)_send_seq(CASC_T_SYNC_BEGIN, c->addr, seq, 0, 0, &p, sizeof(p));

    /* **BEGIN 与第一片之间也要留一个间隔**，与其余分片一样。
     * 对端靠**空闲中断**分帧：两帧首尾相接时线路上不出现空闲，对端会把它们当成
     * **一个块**收下来 —— 接收侧要在一瞬间处理两条帧（这正是当初把深 2 队列压垮的
     * 形态），而且"每帧间隔 1ms"这个前提对第一对不成立，时序再也推不准。
     * 代价 1ms/轮。 */
    osDelay(1);
    _send_frags(c, seq, bmp_len, frag_n, full);
    _casc_drain(); /* NACK（几何不符）可能已经回来了，下面第一圈就会看到 */

    uint8_t mask = full;
    for (uint8_t attempt = 0; attempt <= CASC_RETRY_MAX; attempt++) {
        /* 早先回来的 NACK：几何/参数不符，重发没用 —— 放弃这张卡 */
        if (s_ack.valid && s_ack.src == c->addr && s_ack.seq == seq && s_ack.sta == CASC_STA_NACK) {
            printf("[casc] 卡 %u 拒绝本轮（err=%u），本轮不更新它\n", (unsigned)c->addr,
                   (unsigned)s_ack.miss_mask);
            return false;
        }
        if (attempt) {
            if (!mask) break;
            /* **定向重传**：只补 ACK 报缺的那几片，不是重发整轮 */
            _send_frags(c, seq, bmp_len, frag_n, mask);
        }
        CASC_LOG("[casc·主] seq=%u 卡%u 第%u次：发 %02X 那几片 + COMMIT\n", (unsigned)seq,
                 (unsigned)c->addr, (unsigned)(attempt + 1), (unsigned)mask);
        (void)_send_seq(CASC_T_SYNC_COMMIT, c->addr, seq, 0, 0, nullptr, 0);

        if (!_wait_ack(c->addr, seq, osKernelGetTickCount() + CASC_ACK_TIMEOUT_MS)) {
            CASC_LOG("[casc·主] seq=%u 卡%u ← 等应答超时（%ums）\n", (unsigned)seq,
                     (unsigned)c->addr, (unsigned)CASC_ACK_TIMEOUT_MS);
            /* 超时 = 这张卡没应答，或者 ACK 丢了 —— 现场分不开，只能整卡重来一次 */
            mask = full;
            continue;
        }
        CASC_LOG("[casc·主] seq=%u 卡%u ← sta=%u 缺=%02X\n", (unsigned)seq, (unsigned)c->addr,
                 (unsigned)s_ack.sta, (unsigned)s_ack.miss_mask);

        if (s_ack.sta == CASC_ACK_OK) return true; /* 本轮完成 */
        if (s_ack.sta == CASC_ACK_MISS) {
            mask = s_ack.miss_mask;
            if (!mask) return true; /* 说没缺片却回了 MISS：当完成，别在这儿死循环 */
            continue;
        }
        break; /* NOBEGIN：BEGIN 都没到，多半是链路问题，留到下一轮比在这儿死磕省总线 */
    }

    printf("[casc] 卡 %u 本轮未完成（最后已知缺片位 %02X）—— 其余卡与主卡照常更新\n",
           (unsigned)c->addr, (unsigned)mask);
    return false;
}

/** @brief 开一轮：逐张从卡下发它那一块，全部结算完后主卡自己也换帧 */
static bool _round_run(void)
{
    const screen_layout_t *L      = app_screen_layout();
    const uint8_t          me     = app_screen_self_addr();
    const uint16_t         seq    = ++s_round_seq;
    const uint8_t          bright = app_screen_get_brightness();
    bool                   all_ok = true;

    CASC_LOG("[casc·主] 开轮 seq=%u（共 %u 卡，本卡 addr=%u）\n", (unsigned)seq,
             (unsigned)L->count, (unsigned)me);

    for (uint8_t i = 0; i < L->count; i++) {
        const screen_card_t *c = app_screen_card(i);
        /* **按下标遍历，地址与矩形都从同一项里取** —— 两者顺序可以不同
           （主卡在下时 addr 与下标相反），拿地址当下标会把两块屏的内容对调。
           本卡那块不下发，由下面的本地提交处理。 */
        if (!c || c->addr == me) continue;
        if (!_round_one_card(i, seq, bright)) all_ok = false;
    }

    /* **本地提交放在最后**：主卡自己那块也换到新画面。走的是与从卡完全相同的
       "抽本卡矩形 → commit_bitmap"那条路（app_screen_commit_self），所以主卡屏上的
       内容与从卡收到的出自同一个 app_screen_extract。直接交整块画布不行：多卡时
       画布比本卡屏大，长度对不上，commit_bitmap 会拒绝。

       代价是主卡的换帧时机被总线节奏绑住（一轮 ~130ms/卡）。本项目是交通屏、
       只显示静态文字与标识，指令式刷新，所以不为此另开"只更新主卡本地"的快路径。 */
    (void)app_screen_commit_self();

    return all_ok; /* 只要有一张从卡没完成，上电对齐就还要再试 */
}

#endif /* BOARD_SCREEN_CANVAS */

/* ================================================================
 *  任务
 * ================================================================ */

static void casc_task(void *argument)
{
    (void)argument;

    s_ping_left = CASC_PING_TRIES;
    s_ping_next = osKernelGetTickCount() + CASC_BOOT_PING_DELAY_MS;

    for (;;) {
        /* 阻塞等第一条（超时取轮询周期而不是 osWaitForever：本任务还兼着周期活，
           等不到帧也要醒过来），取到就把队列**一次排空** —— 见 _casc_pump 的说明。 */
        if (_casc_pump(CASC_BRIGHT_POLL_MS)) _casc_drain();

        uint32_t now = osKernelGetTickCount();

        /* 上电枚举的收卷：到点报一句"有没有卡应答" —— 没有应答时后面那些
           "本轮未完成 / 等应答超时"是必然的，这里先把话说在前面，免得白查协议。 */
        if (s_enum_deadline && (int32_t)(now - s_enum_deadline) >= 0) {
            CASC_LOG("[casc·主] 上电枚举：%s\n",
                     s_enum_seen ? "有卡应答（从卡在，链路通）"
                                 : "连发多次 PING 都没有应答 —— 从卡没上电/没接 485/没烧这份"
                                   "固件，或总线方向与接线不对；后面那些「本轮未完成」都是必然的");
            s_enum_deadline = 0;
        }

        /* 上电枚举：从卡通常比主卡晚就绪（等待各自的初始化），故延后 3 秒起发，
           没等到应答就再发 —— 见 CASC_PING_TRIES 的说明。
           运行期的重新枚举（拔插、掉线恢复）留到 P4。 */
        if (app_screen_is_master() && s_ping_left && (int32_t)(now - s_ping_next) >= 0) {
            s_ping_left--;
            s_enum_seen = false; /* 只认**这一轮**发出去之后的应答 */
            (void)app_cascade_ping();
            s_ping_next = now + CASC_PING_RETRY_MS;
            if (!s_ping_left) _enum_finish(now); /* 次数用尽：不再等 */
        }
        /* 收到应答就提前收尾，不必把重试次数耗完 */
        if (app_screen_is_master() && s_ping_left && s_enum_seen) _enum_finish(now);

        /* 亮度待下发：光传感器每秒钟都可能改，这里把"变了"攒成一次广播。
           广播本身**不要求应答** —— 亮度差一帧不可见，且它天然是渐变量。
           "从卡永久停在旧亮度"的防线是 P3 起每一轮 SYNC_BEGIN 都带 bright 重新断言。 */
        uint8_t lv;
        if (app_screen_is_master() && app_screen_brightness_take_pending(&lv)) {
            (void)app_cascade_broadcast_bright(lv);
        }

#if BOARD_SCREEN_CANVAS
        /* 多卡主卡：一轮 = 一次整屏更新，**落屏由它统一做**（app_screen 自己的
           静默期任务在多卡时不启动，见 app_screen.c 的 _screen_init）。
           单卡时整屏就是本卡自己，走那个任务更直接，这儿不成立。
           放在周期活之后：一轮约 130ms/卡，跑起来本任务就顾不上别的了。 */
        if (app_screen_is_master() && app_screen_layout()->count > 1) {
            bool go = false;

            if (s_align_left && (int32_t)(now - s_align_next) >= 0) {
                go = true; /* 上电对齐到点了 */
            } else if (!s_align_left && app_screen_take_pending_settled()) {
                go = true; /* 画布有新内容且已过静默期 */
            }

            if (go) {
                const bool all_ok = _round_run();
                if (s_align_left) {
                    /* 对齐期间**不去消费**待落屏标志：那期间来的渲染留在那儿，
                       等对齐完成后的下一圈自然会开一轮 */
                    if (all_ok) {
                        s_align_left = 0;
                    } else if (--s_align_left) {
                        s_align_next = osKernelGetTickCount() + CASC_BOOT_ALIGN_RETRY_MS;
                    } else {
                        printf("[casc] 上电对齐重试用尽，仍有卡没完成 —— 等下一次内容更新\n");
                    }
                }
            }
        }
#endif
    }
}

/* ================================================================
 *  注册
 * ================================================================ */

static const pcb_ops_t s_casc_ops = {.probe = casc_probe_frame};

static void _cascade_init(void)
{
    pcb_t *p = &s_casc_pcb;
    p->name        = "cascade";
    p->ops         = &s_casc_ops;
    p->rb          = &s_casc_rb;
    /* 不是 CASC_FRAME_MAX：见 CASC_MSG_MAX 的说明 —— 队列元素按它定，设大了
       要么白占 CCMRAM，要么被迫把队列做浅。本协议发出的最长帧就是分片帧。 */
    p->payload_max = CASC_MSG_MAX;

    rb_init(&s_casc_rb, "cascade");

    /* 队列必须在 initcall 内建好，不能等任务启动 —— 否则有"向空队列投递"的窗口 */
    s_casc_queue = osMessageQueueNew(CASC_QUEUE_DEPTH, CASC_MSG_SIZE, &s_casc_queue_attr);
    if (s_casc_queue == nullptr) {
        printf("[casc] 队列创建失败，级联协议停用\n");
        return;
    }
    p->queue = s_casc_queue;

    app_proto_bind(p, app_rs485_ccb());

    const osThreadAttr_t attr = {
        .name       = "cascade",
        .stack_size = 384 * 4,
        .priority   = osPriorityNormal,
    };
    pl_task_new(casc_task, nullptr, &attr);
}
sw_post_initcall(_cascade_init);
