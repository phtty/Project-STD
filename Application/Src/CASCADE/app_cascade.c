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

/* ---- 队列与缓冲区（容量取法见 app_iap.c / app_rls.c 的同名注释）---- */

#define CASC_MSG_SIZE (sizeof(frame_msg_t) + CASC_FRAME_MAX)

static StaticQueue_t s_casc_queue_cb;
static uint8_t       s_casc_queue_buf[2 * CASC_MSG_SIZE] PL_CCMRAM;
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

/** @brief 组一帧到 buf，返回整帧长度；0 表示载荷放不下 */
static uint16_t _build(uint8_t *buf, uint16_t buf_cap, uint8_t type, uint8_t dst, uint16_t seq,
                       const void *payload, uint16_t payload_len)
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
    h->idx    = 0;
    h->frag_n = 0;
    casc_put_u16(h->len, len);

    if (payload_len) memcpy(buf + sizeof(casc_hdr_t), payload, payload_len);

    uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), buf + 2, (size_t)(len - 6U));
    casc_put_u32(buf + len - 4U, crc);
    return len;
}

/** @brief 发一帧。
 *
 *  **缓冲必须 DMA 可达**（RS485 走 DMA 发送），所以用静态 SRAM 缓冲而不是栈上数组 ——
 *  栈在 SRAM 里其实也够得到，但要留一份给将来"发送期间缓冲必须一直有效"的余量，
 *  且静态缓冲让 ccb_send 的阻塞语义不依赖调用栈深度。 */
static int32_t _send(uint8_t type, uint8_t dst, const void *payload, uint16_t payload_len)
{
    static uint8_t s_tx[CASC_FRAME_MAX];
    static uint16_t s_seq;

    uint16_t len = _build(s_tx, sizeof(s_tx), type, dst, s_seq++, payload, payload_len);
    if (!len) return -1;

    ccb_send(app_rs485_ccb(), s_tx, len);
    return (int32_t)len;
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
        (void)_send(CASC_T_NACK, CASC_ADDR_MASTER, &nack, sizeof(nack));
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

    (void)_send(CASC_T_ACK, CASC_ADDR_MASTER, &ack, sizeof(ack));
}

static void _cmd_sync_abort(frame_msg_t *msg)
{
    (void)msg;
    if (app_screen_is_master()) return;
    /* 主卡放弃本轮：清掉暂存，免得半份内容留在那儿被下一轮的 COMMIT 误用 */
    s_rx.active    = false;
    s_rx.have_mask = 0;
}

typedef void (*casc_cmd_fn_t)(frame_msg_t *msg);

/* 按帧类型索引。0 项留空 = 未实现或不支持。
   ACK/NACK 是**主卡侧**的处理，尚未实现（P3b-2）—— 从卡只发不收。 */
static const casc_cmd_fn_t g_casc_cmd[CASC_TYPE_MASK + 1U] = {
    [CASC_T_SYNC_BEGIN]  = _cmd_sync_begin,
    [CASC_T_SYNC_DATA]   = _cmd_sync_data,
    [CASC_T_SYNC_COMMIT] = _cmd_sync_commit,
    [CASC_T_SYNC_ABORT]  = _cmd_sync_abort,
    [CASC_T_PING]        = _cmd_ping,
    [CASC_T_PRESENT]     = _cmd_present,
    [CASC_T_SET_BRIGHT]  = _cmd_set_bright,
};

/* ================================================================
 *  任务
 * ================================================================ */

static void casc_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[CASC_MSG_SIZE];
    frame_msg_t   *msg = (frame_msg_t *)_msg_buf;

    uint32_t next_ping = osKernelGetTickCount() + CASC_BOOT_PING_DELAY_MS;

    for (;;) {
        /* 超时取轮询周期而不是 osWaitForever：本任务还兼着两个周期活
           （上电枚举、亮度待下发），等不到帧也要醒过来。队列深度只有 2，
           而分发任务在队列满时**静默丢帧**（app_dispatch.c 的 osMessageQueuePut
           超时为 0），所以这个超时不能太长 —— P3 会改成"每帧后主动排空"。 */
        if (osMessageQueueGet(s_casc_queue, msg, nullptr, CASC_BRIGHT_POLL_MS) == osOK) {
            /* **取到一条就把队列排空再回去睡**：队列深度只有 2，而框架在队列满时
               **静默丢帧**（app_dispatch.c 的 osMessageQueuePut 超时为 0）。
               一轮图传是"每毫秒一帧"地连发，处理一条就回去睡的话，从第 3 帧起全被
               丢掉 —— 现场表现是"总缺第 2、3 片"，而缺片在从卡侧看不出原因。 */
            do {
                const uint8_t type = msg->aux;
                if (type <= CASC_TYPE_MASK && g_casc_cmd[type]) g_casc_cmd[type](msg);
            } while (osMessageQueueGet(s_casc_queue, msg, nullptr, 0) == osOK);
        }

        uint32_t now = osKernelGetTickCount();

        /* 上电枚举一次：从卡通常比主卡晚就绪（等待各自的初始化），故延后 3 秒。
           运行期的重新枚举（拔插、掉线恢复）留到 P4。 */
        if (app_screen_is_master() && (int32_t)(now - next_ping) >= 0) {
            next_ping = now + 0x7FFFFFFFU; /* 只做一次 */
            (void)app_cascade_ping();
        }

        /* 亮度待下发：光传感器每秒钟都可能改，这里把"变了"攒成一次广播。
           广播本身**不要求应答** —— 亮度差一帧不可见，且它天然是渐变量。
           "从卡永久停在旧亮度"的防线是 P3 起每一轮 SYNC_BEGIN 都带 bright 重新断言。 */
        uint8_t lv;
        if (app_screen_is_master() && app_screen_brightness_take_pending(&lv)) {
            (void)app_cascade_broadcast_bright(lv);
        }
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
    p->payload_max = CASC_FRAME_MAX;

    rb_init(&s_casc_rb, "cascade");

    /* 队列必须在 initcall 内建好，不能等任务启动 —— 否则有"向空队列投递"的窗口 */
    s_casc_queue = osMessageQueueNew(2, CASC_MSG_SIZE, &s_casc_queue_attr);
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
