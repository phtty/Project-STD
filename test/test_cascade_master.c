/**
 * @file    test_cascade_master.c
 * @brief   级联图传 · **主卡侧**：一轮的开轮、整帧下发、结算与整帧重传
 *
 * **为什么需要这个测试**：主卡这一侧错了，现场看到的是"某块屏上不对"或"整轮变慢"，
 * 而没有任何日志会说是哪一步错的。三条只有在这儿才测得到：
 *
 *   1. **地址与矩形错配** —— 协议按地址寻址、切分表按下标索引，两者顺序**可以相反**
 *      （主卡在下时相反）。拿地址当下标就会把两块屏的内容**对调**，而 CRC、长度、
 *      几何校验**全部通过**。用例直接断言"发出去的那帧带的是那张卡自己的矩形"。
 *   2. **重传退化成重开一轮** —— 一帧一轮之后重传就是整帧重发，但**次数必须封顶**，
 *      否则一张掉线的卡会把每一轮都拖长。用例数它**发了几次整帧**。
 *   3. **从卡不回 ACK 时主卡冻结** —— 主卡自己的屏跟着一起不更新，是最坏的失效形态
 *      （整块屏全停）。用例断言"没有任何卡应答时主卡仍然提交本地"。
 *
 * 总线是假的：`ccb_send` 旁边坐着一个**假从卡**，它按协议收 IMAGE、组 ACK/NACK，再经
 * **真探针 + 真队列**喂回主卡 —— 所以走的是真的分帧、真的探针、真的等待循环。
 *
 * app_screen 是替身：抽带与落屏的**正确性**由 test_screen_layout.c 覆盖，
 * 这里只关心"主卡把哪一块发给了谁、发了几次"。边界就划在
 * `app_screen_extract` / `app_screen_commit_self` 上。
 */

#define BOARD_SCREEN_CANVAS 1

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 身份/记录/按键的桩要用到这些类型（本套件不测它们的行为）*/
#include "app_cfg_sched.h"
#include "dev_key.h"
#include "app_cascade.h"
#include "app_screen.h"
#include "dev_display.h"
#include "ring_buffer.h"

/* ---- 本板的单卡几何：位图长度就是 BOARD_CASCADE_BAND_MAX ----
 * 5006048 是 224×50 → 28×50 = 1400；3833024 是 128×32 → 16×32 = 512。 */
#if BOARD_CASCADE_BAND_MAX >= 1400
#define CARD_W (224U)
#define CARD_H (50U)
#else
#define CARD_W (128U)
#define CARD_H (32U)
#endif
#define CARD_BM (BOARD_CASCADE_BAND_MAX)

_Static_assert(((CARD_W + 7U) / 8U) * CARD_H == CARD_BM,
               "本板的单卡几何与 BOARD_CASCADE_BAND_MAX 对不上 —— 新板要在这里补一行");

/* 一帧必须装得下整块位图 —— 这正是"一帧一轮"成立的前提，装不下就别谈删分片 */
_Static_assert(CASC_FRAME_MAX >= CASC_OVERHEAD + sizeof(casc_image_t) + CARD_BM,
               "本板位图一帧装不下");

/* ---- 本卡身份：**夹具可改**（身份用例要换 addr、换主从） ----
 *
 *  主从判据照生产的口径写（`addr == 0`），免得夹具自己造出一套与生产不同的判据，
 *  那样测出来的"对"与屏上的"对"是两回事。 */
static uint8_t s_self_addr = 0; /* 出厂默认：主卡 */

uint8_t app_screen_index_of_addr(uint8_t a); /* 前置声明：定义在下面 */

bool    app_screen_is_master(void) { return s_self_addr == CASC_ADDR_MASTER; }
uint8_t app_screen_self_addr(void) { return s_self_addr; }
void    app_screen_set_addr(uint8_t a) { s_self_addr = a; }

static int s_reinit_calls;
void       app_screen_reinit_identity(void) { s_reinit_calls++; }

/* ---- app_screen 替身 ---- */

/* **按现场的真实拓扑排**：上下拼，**下面那块是主卡**。
   于是 下标 0 是上面的从卡（addr 1）、下标 1 是下面的主卡（addr 0）——
   **addr 与下标正好相反**，这正是"拿地址当下标"最容易出错、也最该被测到的形态。
   （若按 addr==下标 排，那种错法在这套用例里完全测不出来。） */
#define SELF_IDX (1U) /* 本卡（主卡 addr 0）落在下标 1 */

static screen_card_t s_cards[2] = {
    {.addr = 1, .color = COLOR_RED, .x = 0, .y = 0, .w = CARD_W, .h = CARD_H}, /* 上：从卡 */
    {.addr = 0, .color = COLOR_GREEN, .x = 0, .y = CARD_H, .w = CARD_W, .h = CARD_H}, /* 下：主卡 */
};
static const screen_layout_t s_layout = {
    .cards = s_cards,
    .count = 2,
    .rows  = CARD_W,
    .cols  = (uint16_t)(CARD_H * 2),
};

const screen_layout_t *app_screen_layout(void) { return &s_layout; }
const screen_card_t   *app_screen_card(uint8_t idx) { return (idx < 2) ? &s_cards[idx] : nullptr; }
uint16_t               app_screen_card_bm_len(uint8_t idx) { return (idx < 2) ? CARD_BM : 0; }
uint8_t app_screen_self_index(void)
{
    return app_screen_index_of_addr(s_self_addr); /* 身份一改，"我是哪一格"跟着走 */
}

/* ---- 卡片状态：**忠实实现**，不是空桩 ----
 *
 * P4 的策略（不在线就跳过、连续失败到阈值剔除、收到 PRESENT 就回到在线）全靠这几个
 * 函数与轮次配合。给空桩的话，那几条策略在这套用例里就一条也测不到。 */
static uint8_t s_state[2] = {SCREEN_CARD_MISSING, SCREEN_CARD_MISSING};
static uint16_t s_last_seq;
static int      s_retrans_cnt;

screen_card_state_t app_screen_card_state(uint8_t i)
{
    return (i < 2) ? (screen_card_state_t)s_state[i] : SCREEN_CARD_MISSING;
}
void app_screen_card_set_state(uint8_t i, screen_card_state_t st)
{
    if (i < 2) s_state[i] = (uint8_t)st;
}
uint8_t app_screen_index_of_addr(uint8_t a)
{
    for (uint8_t i = 0; i < 2; i++)
        if (s_cards[i].addr == a) return i;
    return 0xFF;
}
void app_screen_note_round(uint16_t seq) { s_last_seq = seq; }
void app_screen_note_retrans(void) { s_retrans_cnt++; }

/* ---- 身份记录：**一小块存储器**（用例据此构造"有记录 / 无记录 / 读失败"） ----
 *
 *  桩成"永远读不到"就只能测出厂默认那一条；而身份的来源有三档、优先级才是被测的东西。 */
static bool           s_rec_present;                          /* 有没有记录 */
static uint8_t        s_rec_addr, s_rec_src;                  /* 记录里的内容 */
static cfg_rec_sta_t  s_load_sta = CFG_REC_OK;                /* 用例可摆成 IO_ERR */
static int            s_load_calls;
static int            s_save_calls;
static uint8_t        s_last_saved_addr, s_last_saved_src;

uint8_t app_cfg_sched_register(const cfg_sched_desc_t *d)
{
    (void)d;
    return 0; /* 有效句柄：注册失败的话身份就永远不落盘，那是另一条用例 */
}
cfg_rec_sta_t app_cfg_sched_load(uint8_t id, uint8_t *p, uint16_t c, uint16_t *l)
{
    (void)id;
    s_load_calls++;
    if (s_load_sta != CFG_REC_OK) return s_load_sta; /* IO_ERR：**必须**原样上报，不许被缓存 */
    if (!s_rec_present) return CFG_REC_EMPTY;
    if (c < 4) return CFG_REC_EMPTY;
    p[0] = s_rec_addr; /* 记录体 = {addr, src, rsv[2]} */
    p[1] = s_rec_src;
    p[2] = 0;
    p[3] = 0;
    *l   = 4;
    return CFG_REC_OK;
}
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *p, uint16_t n)
{
    (void)id;
    if (n >= 2) {
        s_save_calls++;
        s_last_saved_addr = p[0];
        s_last_saved_src  = p[1];
    }
    return 0;
}

/* ---- 拨码：用例可摆出"本板有拨码、值是几"（只有 BOARD_HAS_ADDR_DIP=1 的板会读它） ---- */
static dev_key_t s_dip_key;
static bool      s_has_dip;
static bool      s_dip_bit[2];

static bool _dip1_state(dev_key_t *k)
{
    (void)k;
    return s_dip_bit[0];
}
static bool _dip2_state(dev_key_t *k)
{
    (void)k;
    return s_dip_bit[1];
}
static const dev_key_ops_t s_dip1_ops = {.get_state = _dip1_state};
static const dev_key_ops_t s_dip2_ops = {.get_state = _dip2_state};

dev_key_t *dev_key_get(dev_key_id_t id)
{
    if (!s_has_dip) return nullptr;
    if (id == DEV_KEY_DIP1) {
        s_dip_key.ops = &s_dip1_ops;
        return &s_dip_key;
    }
    if (id == DEV_KEY_DIP2) {
        s_dip_key.ops = &s_dip2_ops;
        return &s_dip_key;
    }
    return nullptr;
}

/* 从卡落盘与开轮时 peek 的持久化请求位：本套件桩成"从不请求持久化" */
void app_render_save(void) {}
bool app_render_peek_persist_req(void) { return false; }
uint8_t app_screen_output_color(uint8_t c) { return c; } /* 无颜色覆盖 */

/* ---- 开轮三闸的输入：用例逐个摆（`_round_ready` 就是靠这三个判的） ---- */
static bool s_canvas_touched, s_settled, s_render_busy;

bool app_screen_canvas_touched(void) { return s_canvas_touched; }
bool app_render_busy(void) { return s_render_busy; }
bool app_screen_take_pending_settled(void)
{
    if (!s_settled) return false;
    s_settled = false; /* 与生产同语义：取了就清 */
    return true;
}


/** 每张卡一块可辨认的图案 —— 张冠李戴（把 A 卡的矩形发给 B 卡）当场露馅 */
static void card_pattern(uint8_t idx, uint8_t *out, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) out[i] = (uint8_t)((idx << 6) ^ (i * 7U + 3U));
}

static int s_extract_calls;

bool app_screen_extract(uint8_t card_idx, uint8_t *buf, uint16_t cap)
{
    if (card_idx >= 2 || !buf || cap < CARD_BM) return false;
    s_extract_calls++;
    card_pattern(card_idx, buf, CARD_BM);
    return true;
}

static int s_commit_self_calls;
bool      app_screen_commit_self(void)
{
    s_commit_self_calls++;
    return true;
}

void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color)
{
    (void)bm;
    (void)len;
    (void)color;
}
void    app_screen_set_brightness(uint8_t l) { (void)l; }
uint8_t app_screen_get_brightness(void) { return 3; }
bool    app_screen_brightness_take_pending(uint8_t *l)
{
    (void)l;
    return false;
}
/* ---- 显示与总线 ---- */
#define DEV_W (48U)
#define DEV_H (16U)
static dev_display_t s_dev;
static uint8_t       s_pixel_map[DEV_W * DEV_H];
dev_display_t       *dev_display_get(void) { return &s_dev; }

/* ---- 被测：生产源码本体 ---- */
#include "../Application/Src/CASCADE/app_cascade.c"

/* ================================================================
 *  假从卡 —— 坐在总线另一头，按协议收 IMAGE、组 ACK
 * ================================================================ */

static struct {
    /** 收到过几条 IMAGE（**重整帧重传靠它验证**：一帧一轮，重传就是整帧重发） */
    int      image_rx;
    uint16_t seq;     /**< 最近一条 IMAGE 的轮次序号 */
    uint16_t bmp_len; /**< 最近一条 IMAGE 声明的位图长度 */
    uint8_t  stage[CARD_BM]; /**< 收到的位图（与主卡抽给**本卡**的那块比对） */
    bool     applied;        /**< 有内容落下来过 */

    /* 注入 */
    bool drop_first;  /**< **只丢首发**（模拟总线瞬时丢帧，重传能过） */
    bool no_apply;    /**< 收到了但落不下去（也不回 ACK）—— 持续干扰/卡忙 */
    bool silent;      /**< 连收都没收到（卡掉线）：一句话都不回 */
    bool nack;        /**< 明确回绝（模拟矩形与本卡切分表不符） */
    bool nack_addr;   /**< 拒绝识别帧（模拟"本卡有拨码、地址由拨码定"） */

    /* 识别帧（认领时逐卡单播给本卡） */
    int             set_addr_rx;
    casc_set_addr_t set_addr_payload;

    /* 观察 */
    uint8_t last_x[2]; /**< 最近一条 IMAGE 里的矩形 x（大端） */
    uint8_t last_y[2];
    uint8_t last_w[2];
    uint8_t last_h[2];
    uint16_t last_frame_len;
} s_slave;

static void slave_reset(void)
{
    memset(&s_slave, 0, sizeof(s_slave));
}

/* ---- 把从卡发出的帧喂回主卡：真探针 → 并入协议队列 ---- */

RB_DEFINE(s_rb, 4096);

/** @brief 相当于 frame_dispatch_task 的那一圈：探针 → rb_read → 入队
 *
 *  真框架是"等通道通知 → 遍历该通道的协议 → 探针 → rb_read → osMessageQueuePut"。
 *  本用例没有通道线程，所以由假从卡在"回了一帧之后"手动跑这一圈。 */
static void dispatch_pending(void)
{
    static uint8_t     buf[sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));
    frame_msg_t *const msg = (frame_msg_t *)buf;

    for (;;) {
        uint32_t        tl  = 0;
        uint8_t         aux = 0;
        pcb_probe_sta_t st  = casc_probe_frame(&s_casc_pcb, nullptr, nullptr, msg->data,
                                               FRAME_DATA_MAX_LEN, &tl, &aux);
        if (st == PCB_PROBE_FAKE) {
            rb_skip(&s_rb, 1, nullptr);
            continue;
        }
        if (st != PCB_PROBE_READY) return;

        rb_skip(&s_rb, tl, nullptr);
        msg->data_len = (uint16_t)tl;
        msg->aux      = aux;
        msg->ccb      = nullptr;
        osMessageQueuePut(s_casc_pcb.queue, msg, 0, 0);
    }
}

/** @brief 总线另一头发一帧给本卡（收/发地址都要能点名 —— 识别帧来自**另一张主卡**，
 *         它的 src 是 0，而从卡的应答 src 是 1） */
static void peer_frame(uint8_t src, uint8_t dst, uint8_t type, uint16_t seq, const void *payload,
                       uint16_t plen)
{
    uint8_t        f[CASC_FRAME_MAX];
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(f, 0, len);
    f[0]            = CASC_SOF0;
    f[1]            = CASC_SOF1;
    f[2]            = (uint8_t)((CASC_PROTO_VER << 6) | type);
    f[3]            = dst;
    f[4]            = src;
    casc_put_u16(f + 5, seq);
    casc_put_u16(f + 9, len);
    if (plen) memcpy(f + 11, payload, plen);
    casc_put_u32(f + len - 4U, pl_crc32_calc(pl_crc_get_handle(), f + 2, len - 6U));

    rb_write(&s_rb, f, len, nullptr);
    dispatch_pending();
}

/** @brief 假从卡回一帧给主卡（dst=0, src=1） */
static void slave_reply(uint8_t type, uint16_t seq, const void *payload, uint16_t plen)
{
    peer_frame(1, CASC_ADDR_MASTER, type, seq, payload, plen);
}

/** @brief 组一条识别帧的载荷（`mine`/`yours`/`claim` 都由用例给） */
static casc_set_addr_t set_addr_payload(uint8_t mine, uint8_t yours, uint32_t claim)
{
    casc_set_addr_t p = {.mine = mine, .yours = yours};
    casc_put_u32(p.claim, claim);
    return p;
}

static void slave_send_present(void)
{
    casc_present_t p;
    memset(&p, 0, sizeof(p));
    p.addr = 1;
    casc_put_u16(p.w, CARD_W);
    casc_put_u16(p.h, CARD_H);
    p.bright    = 3;
    p.proto_ver = CASC_PROTO_VER;
    slave_reply(CASC_T_PRESENT, 1, &p, sizeof(p));
}

/** @brief 回一条 ACK（**无载荷**：主卡只按 (seq, src) 认它） */
static void slave_send_ack(uint16_t seq) { slave_reply(CASC_T_ACK, seq, nullptr, 0); }

static uint8_t s_tx_count; /**< 主卡一共发了多少帧 */

/** 本卡最近发出去的一帧（身份用例要断言"回了什么"：ACK / NACK(ADDR)） */
static struct {
    uint8_t  type, dst, src;
    uint16_t seq, len;
    uint8_t  payload[CASC_FRAME_MAX];
} s_tx_last;

/** 假从卡收到 IMAGE 之后把本卡身份改成这个值（-1 = 不改）—— 用来构造
 *  "一轮中途身份变了"，那是 `s_round_me` 那条收尾逻辑的唯一触发条件。 */
static int s_change_addr_after_rx = -1;

/** @brief 假从卡：收主卡发来的帧 */
static void slave_on_master_frame(const uint8_t *d, uint16_t l)
{
    const uint8_t  type = CASC_TYPE_OF(d[2]);
    const uint16_t seq  = casc_get_u16(d + 5);

    /* 总线是共享的，但**不是发给本卡的帧一律不理** —— 与真从卡一样。
       少了这条，"主卡把帧发给了自己（dst=0）"这种错会被当成正常收到。 */
    if (d[3] != 1) return;

    /* ---- 识别帧（认领主卡时逐卡单播给本卡） ---- */
    if (type == CASC_T_SET_ADDR) {
        s_slave.set_addr_rx++;
        memset(&s_slave.set_addr_payload, 0, sizeof(s_slave.set_addr_payload));
        const uint16_t plen = (uint16_t)(l - CASC_OVERHEAD);
        memcpy(&s_slave.set_addr_payload, d + sizeof(casc_hdr_t),
               plen < sizeof(s_slave.set_addr_payload) ? plen : sizeof(s_slave.set_addr_payload));
        if (s_slave.nack_addr) {
            const casc_nack_t n = {.err = CASC_NACK_ADDR};
            slave_reply(CASC_T_NACK, seq, &n, sizeof(n));
        } else {
            slave_send_ack(seq);
        }
        return;
    }

    if (type != CASC_T_IMAGE) return;

    s_slave.image_rx++;
    s_slave.seq           = seq;
    s_slave.last_frame_len = l;

    /* 一轮中途换身份（真正的触发点在"发帧"这一瞬，也就是 _round_one_card 还在跑的时候） */
    if (s_change_addr_after_rx >= 0) s_self_addr = (uint8_t)s_change_addr_after_rx;

    const casc_image_t *p = (const casc_image_t *)(d + sizeof(casc_hdr_t));
    s_slave.bmp_len       = casc_get_u16(p->bmp_len);
    memcpy(s_slave.last_x, p->x, 2);
    memcpy(s_slave.last_y, p->y, 2);
    memcpy(s_slave.last_w, p->w, 2);
    memcpy(s_slave.last_h, p->h, 2);

    if (s_slave.nack) { /* 矩形与本卡切分表不符：明确拒绝，重发没用 */
        const casc_nack_t n = {.err = CASC_NACK_GEOM};
        slave_reply(CASC_T_NACK, seq, &n, sizeof(n));
        return;
    }
    if (s_slave.silent) return;              /* 卡掉线：连收都没收到 */
    if (s_slave.no_apply) return;            /* 收到了但落不下去，也就不该回 ACK */
    if (s_slave.drop_first && s_slave.image_rx == 1) return; /* 只丢首发 */

    /* 校验帧长自洽（真从卡也会这么做），然后收下整幅 */
    if (l != (uint16_t)(CASC_OVERHEAD + sizeof(casc_image_t) + s_slave.bmp_len)) return;
    if (s_slave.bmp_len != CARD_BM) return;

    memcpy(s_slave.stage, p->bitmap, s_slave.bmp_len);
    s_slave.applied = true;
    slave_send_ack(seq);
}

int32_t ccb_send(ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    s_tx_count++;
    if (l >= CASC_OVERHEAD) {
        memset(&s_tx_last, 0, sizeof(s_tx_last));
        s_tx_last.type = CASC_TYPE_OF(d[2]);
        s_tx_last.dst  = d[3];
        s_tx_last.src  = d[4];
        s_tx_last.seq  = casc_get_u16(d + 5);
        s_tx_last.len  = l;
        if (l - CASC_OVERHEAD <= sizeof(s_tx_last.payload))
            memcpy(s_tx_last.payload, d + sizeof(casc_hdr_t), (size_t)(l - CASC_OVERHEAD));
        slave_on_master_frame(d, l);
    }
    return (int32_t)l;
}
ccb_t *app_rs485_ccb(void) { return nullptr; }
void   app_proto_bind(pcb_t *p, ccb_t *c) { (void)p; (void)c; }
void   pl_task_new_stub(void) {}

/* ================================================================
 *  夹具与断言
 * ================================================================ */

static void fixture_reset(void)
{
    s_rb.read_index  = 0;
    s_rb.write_index = 0;
    s_casc_pcb.rb    = &s_rb;

    /* **两处都要指到同一个队列**：假从卡往 pcb.queue 里投（那是框架投递的目标），
       而主卡的排空读的是 s_casc_queue（`_cascade_init` 里那一份）——
       本用例没跑 initcall，所以得自己把两者接上。只接一处的话 ACK 进了没人读的
       队列，表现是"每张卡都超时"，与"卡掉线"完全一样。 */
    s_casc_queue     = osMessageQueueNew(2, CASC_MSG_SIZE, nullptr);
    s_casc_pcb.queue = s_casc_queue;

    s_tx_count          = 0;
    s_extract_calls     = 0;
    s_commit_self_calls = 0;
    s_ack.valid         = false;

    /* 身份/记录/拨码：一律回出厂态（无记录、无拨码、主卡） */
    s_self_addr            = CASC_ADDR_MASTER;
    s_reinit_calls         = 0;
    s_rec_present          = false;
    s_rec_addr             = 0;
    s_rec_src              = 0;
    s_load_sta             = CFG_REC_OK;
    s_load_calls           = 0;
    s_save_calls           = 0;
    s_last_saved_addr      = 0xFF;
    s_last_saved_src       = 0xFF;
    s_has_dip              = false;
    s_dip_bit[0]           = false;
    s_dip_bit[1]           = false;
    s_change_addr_after_rx = -1;
    s_my_claim             = 0xFFFFFFFFU;
    s_canvas_touched       = false;
    s_settled              = false;
    s_render_busy          = false;
    s_claim_req            = false;
    memset(&s_tx_last, 0, sizeof(s_tx_last));

    /* 身份记录的**注册**在生产里是 initcall（host 上不跑），不补这一下 `s_id_cfg`
       就一直是 0xFF —— 那样所有落盘路径都被静默跳过，用例测的就不是它要测的东西了。 */
    _casc_id_register();

    /* 默认：从卡在线。**不在线的卡会被轮次直接跳过**，所以每条用例都得先把它置在线，
       否则测的就不是它本来要测的东西了。 */
    s_state[0]      = SCREEN_CARD_ONLINE;
    s_state[1]      = SCREEN_CARD_ONLINE;
    s_last_seq      = 0;
    s_retrans_cnt   = 0;
    s_force_round   = false;
    /* 失败计数是 app_cascade.c 的文件级静态，**跨用例会累积** —— 不重置的话
       几条"让从卡不回 ACK"的用例合起来就把卡剔除了，后面的用例全跟着变。 */
    for (uint8_t k = 0; k < SCREEN_CARD_MAX; k++) s_fail_run[k] = 0;

    s_dev.screen_rows = DEV_W;
    s_dev.screen_cols = DEV_H;
    s_dev.pixel_map   = s_pixel_map;

    slave_reset();
}

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/** @brief 从卡收到的内容必须等于"主卡抽给**这张卡**的那一块" */
static void check_content_is_card(uint8_t card_idx, const char *what)
{
    uint8_t expect[CARD_BM];
    card_pattern(card_idx, expect, CARD_BM);
    CHECK_MSG(s_slave.applied, "%s：从卡什么都没收到", what);
    CHECK_MSG(memcmp(s_slave.stage, expect, CARD_BM) == 0, "%s：收到的内容与主卡抽出的不一致",
              what);
}

/* ================================================================ */

/** 正常一轮：整块位图一条帧到齐，从卡收下并应答，主卡提交本地 */
static void case_round_ok(void)
{
    TEST_BEGIN("正常一轮：一条 IMAGE 装下整块位图，从卡收下、主卡提交本地");

    fixture_reset();
    /* 返回值是给"上电对齐"用的：全部从卡都完成才算成 */
    CHECK_MSG(_round_run(), "全部从卡完成时应返回 true（上电对齐据此决定还要不要再试）");

    CHECK_MSG(s_slave.image_rx == 1, "从卡应收到 1 条 IMAGE，得到 %d", s_slave.image_rx);
    CHECK_MSG(s_slave.bmp_len == CARD_BM, "从卡理解的位图长度应为 %u，得到 %u",
              (unsigned)CARD_BM, (unsigned)s_slave.bmp_len);
    CHECK_MSG(s_slave.last_frame_len == (uint16_t)(CASC_OVERHEAD + sizeof(casc_image_t) + CARD_BM),
              "整帧长度应为 %u，得到 %u",
              (unsigned)(CASC_OVERHEAD + sizeof(casc_image_t) + CARD_BM),
              (unsigned)s_slave.last_frame_len);
    check_content_is_card(0, "正常一轮");

    CHECK_MSG(s_commit_self_calls == 1, "主卡应提交本地一次，得到 %d", s_commit_self_calls);
}

/** 发出去的那帧必须带**那张卡自己**的矩形 —— 地址与下标不许错配 */
static void case_rect_matches_addr(void)
{
    TEST_BEGIN("IMAGE 带的是该卡自己的矩形（地址与下标不许错配）");

    fixture_reset();
    _round_run();

    /* 发给 addr 1 的必须是**它自己**那块（下标 0 = 上半屏 y=0），
       而不是主卡那块（下标 SELF_IDX = 下半屏 y=CARD_H）。 */
    const uint16_t y = casc_get_u16(s_slave.last_y);
    CHECK_MSG(y == 0, "发给 addr 1 的矩形 y 应为 0（上半屏），得到 %u（=%u 说明把主卡那块发过去了）",
              (unsigned)y, (unsigned)CARD_H);
    CHECK_MSG(casc_get_u16(s_slave.last_w) == CARD_W && casc_get_u16(s_slave.last_h) == CARD_H,
              "矩形尺寸应是该卡自己的屏几何");

    /* 内容是"抽给这张卡的"而不是"抽给主卡自己的" —— 两张卡的图案不同，能分辨 */
    uint8_t expect_self[CARD_BM];
    uint8_t expect_slave[CARD_BM];
    card_pattern(SELF_IDX, expect_self, CARD_BM);
    card_pattern(0, expect_slave, CARD_BM);
    CHECK_MSG(memcmp(expect_self, expect_slave, CARD_BM) != 0, "本夹具两张卡的图案必须不同");
    check_content_is_card(0, "地址与下标错配的检查");

    /* 自检：**addr 与下标必须不同序、两卡矩形必须不同位** ——
       否则"发错卡"和"拿地址当下标"这两条根本测不出来。 */
    CHECK_MSG(s_cards[0].addr != 0 && s_cards[SELF_IDX].addr == 0,
              "本夹具必须让 addr 与下标**不同序**，否则这条用例没有分辨力");
    CHECK_MSG(s_cards[0].y != s_cards[SELF_IDX].y,
              "本夹具两张卡的矩形必须**不同位**，否则「发错卡」测不出来");

    /* 本卡（addr 0）那块**不该**被下发 —— 它由本地提交处理 */
    CHECK_MSG(s_extract_calls == 1, "只应抽取从卡那一块（本卡那块走 commit_self），抽了 %d 次",
              s_extract_calls);
}

/** 丢一条：整帧重发（一帧一轮之后没有"只补某一片"这种粒度） */
static void case_retransmit_whole_frame(void)
{
    TEST_BEGIN("丢一条 IMAGE → 整帧重发，补上后从卡内容正确");

    fixture_reset();
    s_slave.drop_first = true;
    (void)_round_run();

    CHECK_MSG(s_slave.image_rx == 2, "首发丢了应整帧重发一次（共 2 条），得到 %d",
              s_slave.image_rx);
    check_content_is_card(0, "整帧重发");
    CHECK_MSG(s_commit_self_calls == 1, "重发成功后主卡应提交本地");
}

/** 从卡始终不回 ACK：主卡**不许冻结**，本地照常更新，且重试必须封顶 */
static void case_silent_slave_master_still_commits(void)
{
    TEST_BEGIN("从卡不回 ACK → 主卡仍提交本地（整块屏不许跟着停）");

    fixture_reset();
    s_slave.silent = true;

    const uint32_t t0 = osKernelGetTickCount();
    CHECK_MSG(!_round_run(), "从卡没完成时应返回 false（上电对齐据此重试）");
    const uint32_t dt = osKernelGetTickCount() - t0;

    CHECK_MSG(s_commit_self_calls == 1, "从卡一句话不回时，主卡**仍然**要提交本地，得到 %d 次",
              s_commit_self_calls);

    /* 重试有限度：一次首发 + CASC_RETRY_MAX 次重传，不能无限试下去 */
    const int want = 1 + (int)CASC_RETRY_MAX;
    CHECK_MSG(s_slave.image_rx == want, "重传次数应被 CASC_RETRY_MAX 封顶：期望 %d 条，得到 %d",
              want, s_slave.image_rx);
    CHECK_MSG(dt < 1000U, "一轮耗时应在一秒内（卡掉线时也不该拖长），实测 %ums",
              (unsigned)dt);
}

/** 一直落不下去：重试封顶后放弃该卡，主卡自己照常更新 */
static void case_permanent_loss_gives_up(void)
{
    TEST_BEGIN("持续落不下去 → 重试封顶后放弃，主卡不跟着卡死");

    fixture_reset();
    s_slave.no_apply = true;

    const uint32_t t0 = osKernelGetTickCount();
    (void)_round_run();
    const uint32_t dt = osKernelGetTickCount() - t0;

    CHECK_MSG(!s_slave.applied, "本用例里从卡不该落屏成功");
    CHECK_MSG(s_slave.image_rx == 1 + (int)CASC_RETRY_MAX, "应重试到封顶，得到 %d 条",
              s_slave.image_rx);
    CHECK_MSG(s_commit_self_calls == 1, "一张卡补不上不该拖住主卡自己的更新");
    CHECK_MSG(dt < 1000U, "放弃要及时（重试封顶），实测 %ums", (unsigned)dt);
}

/** 从卡回 NACK（几何不符）：立刻放弃这张卡，**不做无用的重传** */
static void case_nack_gives_up(void)
{
    TEST_BEGIN("从卡回 NACK → 立刻放弃该卡，不做无用的重传");

    fixture_reset();
    s_slave.nack = true;
    (void)_round_run();

    CHECK_MSG(s_slave.image_rx == 1, "被 NACK 的卡不该收到重发的帧（重发也还是不符），得到 %d 条",
              s_slave.image_rx);
    CHECK_MSG(s_commit_self_calls == 1, "一张卡参与不了，主卡自己仍要更新画面");
}

/** 整轮不许重复抽取：每张从卡一次（重发用的是已经抽好的那帧） */
static void case_one_extract_per_card(void)
{
    TEST_BEGIN("每张从卡只抽一次（重发用的是已经组好的那帧）");

    fixture_reset();
    s_slave.drop_first = true;
    (void)_round_run();

    CHECK_MSG(s_extract_calls == 1, "一次抽取 + 一次整帧重发也只该抽 1 次，抽了 %d 次",
              s_extract_calls);
}

/** 不在线的卡一帧都不发；连续失败到阈值才剔除；PRESENT 回来立刻复在线 */
static void case_evict_skip_recover(void)
{
    TEST_BEGIN("跳过不在线 / 连续失败才剔除 / 收到 PRESENT 立刻复在线");

    /* ---- 从未应答过（MISSING）：一帧都不发 ---- */
    fixture_reset();
    s_state[0] = SCREEN_CARD_MISSING;
    /* **没上线的卡也算"这轮没对上"** —— 否则从卡晚几秒起来时，对齐全在"没人应答"
       的状态下判成成功、只走一轮就收尾，上电同步永远不会发生。 */
    CHECK_MSG(!_round_run(), "有卡还没上线时，这一轮不该算对齐完成");
    CHECK_MSG(s_tx_count == 0, "不在线的卡不该收到任何帧，却发了 %u 帧", (unsigned)s_tx_count);
    CHECK_MSG(s_commit_self_calls == 1, "没有从卡可发时，主卡自己也该照常更新");

    /* ---- 连续失败到阈值才剔除 ---- */
    fixture_reset();
    s_slave.silent = true;
    for (uint8_t r = 1; r < CASC_FAIL_RUN_MAX; r++) {
        (void)_round_run();
        CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_ONLINE,
                  "第 %u 轮后不该剔除 —— 单轮失败多半是偶发冲突，下一轮自己就好，"
                  "剔了反而要等下一次枚举才叫得回来",
                  (unsigned)r);
    }
    (void)_round_run();
    CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_OFFLINE, "连续 %u 轮未完成应剔除",
              (unsigned)CASC_FAIL_RUN_MAX);

    /* ---- 被剔除的卡：**一帧都不再发**（这是剔除的全部收益）---- */
    const uint8_t before = s_tx_count;
    (void)_round_run();
    CHECK_MSG(s_tx_count == before, "被剔除的卡不该再收到任何帧，却发了 %u 帧",
              (unsigned)(s_tx_count - before));
    CHECK_MSG(s_commit_self_calls >= 1, "剔除一张卡不该拖住主卡自己的更新");

    /* ---- 它回来了：PRESENT → 立刻复在线，并要求马上开一轮 ---- */
    s_state[0]    = SCREEN_CARD_OFFLINE;
    s_force_round = false;
    slave_send_present();
    _casc_drain();
    CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_ONLINE, "收到 PRESENT 应复在线");
    CHECK_MSG(s_force_round,
              "刚上线的卡应请求**立刻**开一轮 —— 它屏上还是掉线前那幅旧的，"
              "等下一次内容更新可能要几分钟");
}

/** 整帧重传会计数 —— 状态快照要用它 */
static void case_retrans_counted(void)
{
    TEST_BEGIN("整帧重传计入快照计数器");

    fixture_reset();
    s_slave.drop_first = true;
    (void)_round_run();
    CHECK_MSG(s_retrans_cnt == 1, "重发了一次，计数应为 1，得到 %d", s_retrans_cnt);

    fixture_reset();
    (void)_round_run();
    CHECK_MSG(s_retrans_cnt == 0, "没重发时不该有计数，得到 %d", s_retrans_cnt);
}

/* ================================================================
 *  身份：从哪读、收到识别帧怎么办、认领之后做什么
 *
 *  这三段是"谁被按谁是主卡"的全部实现，而它们的错法**都不会报错**：身份读错
 *  只是两块屏的角色不对（画面照样出），识别帧判错只是"按了没反应"。
 *  三档来源的优先级、回声过滤、拨码优先、认领后的状态作废，只有在这儿才看得见。
 * ================================================================ */

/** 身份三档来源的优先级：拨码 > 记录 > 板级默认 */
static void case_id_resolve_sources(void)
{
    TEST_BEGIN("身份来源：拨码 > 记录 > 板级默认（出厂态 = 主卡）");

    /* ① 出厂态：无记录、无拨码 → 板级默认 */
    fixture_reset();
    _casc_id_boot();
    CHECK_MSG(s_self_addr == (uint8_t)BOARD_CASCADE_ADDR,
              "出厂态应取板级默认 %u，得到 %u", (unsigned)BOARD_CASCADE_ADDR,
              (unsigned)s_self_addr);
    CHECK_MSG(s_reinit_calls == 1, "解析出身份后必须重装门面一次，得到 %d 次", s_reinit_calls);

    /* ② 有记录 → 记录优先于默认（先把本机摆成别的，证明真的读了记录） */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 1;
    s_rec_src     = 9;
    s_self_addr   = 0; /* 摆成与记录不同的值 */
    _casc_id_boot();
    CHECK_MSG(s_self_addr == 1, "有记录时应取记录里的 1，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_reinit_calls == 1, "读记录这条也要重装门面");

    /* ③ 记录越界（> 0x1F 的地址根本不存在）→ **不许采纳**，回落默认 */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 0x40;
    _casc_id_boot();
    CHECK_MSG(s_self_addr == (uint8_t)BOARD_CASCADE_ADDR,
              "越界记录必须被拒、回落到默认 %u，得到 %u", (unsigned)BOARD_CASCADE_ADDR,
              (unsigned)s_self_addr);

    /* ④ 读失败（IO_ERR）**不许被缓存**：每次上电/每次解析都要去问一次，
       否则一次瞬时读失败会让身份永远停在默认值上 */
    fixture_reset();
    s_load_sta = CFG_REC_IO_ERR;
    _casc_id_boot();
    _casc_id_boot();
    CHECK_MSG(s_load_calls == 2, "IO_ERR 不该被缓存：两次解析应两次都去读，得到 %d 次",
              s_load_calls);

#if BOARD_HAS_ADDR_DIP
    /* ⑤ 有拨码 → **拨码优先**，且不落盘（现场拨一下即生效，不需要工具） */
    fixture_reset();
    s_has_dip   = true;
    s_dip_bit[0] = true; /* DIP1 = bit0，ON = 低 = 1 */
    s_dip_bit[1] = false;
    s_rec_present = true;
    s_rec_addr    = 5; /* 记录说是 5，拨码说是 1 —— 必须听拨码 */
    _casc_id_boot();
    CHECK_MSG(s_self_addr == 1, "拨码=1 时身份应是 1（拨码优先于记录里的 5），得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "拨码板上电不该写身份记录（地址由拨码定）");

    /* ⑥ 拨码是**每次现读**：现场拨到 2，下一次解析立刻跟上 */
    s_dip_bit[0] = false;
    s_dip_bit[1] = true;
    _casc_id_boot();
    CHECK_MSG(s_self_addr == 2, "拨码改到 2 后应立刻生效，得到 %u", (unsigned)s_self_addr);
#else
    /* ⑤ 本板无拨码（BOARD_HAS_ADDR_DIP=0）：**即使挂着拨码设备也不读** ——
       5006048 的地址来源只有记录与默认，这个编译期开关写错会让两块板定址方式串味 */
    fixture_reset();
    s_has_dip     = true;
    s_dip_bit[0]  = true;
    s_rec_present = true;
    s_rec_addr    = 1;
    _casc_id_boot();
    CHECK_MSG(s_self_addr == 1, "本板不读拨码，应取记录里的 1，得到 %u", (unsigned)s_self_addr);
#endif
}

/** 收到识别帧：四个分支 + 回声过滤（一个都不许判错） */
static void case_set_addr_branches(void)
{
    TEST_BEGIN("识别帧：非主卡拒绝 / 只确认角色 / 从卡改址 / 回声丢弃");

    /* ① `mine != 0`：对方不是主卡 → 拒绝，本机身份不动。
       这一条守住"整个装置不会没有主卡"：从卡按键不能把唯一的主卡降级。 */
    fixture_reset();
    casc_set_addr_t p = set_addr_payload(1, 2, 100);
    peer_frame(1, CASC_ADDR_MASTER, CASC_T_SET_ADDR, 11, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 0, "非主卡的识别帧不该改本机身份，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "被拒绝的识别帧不该写记录");
    CHECK_MSG(s_tx_last.type == CASC_T_NACK && s_tx_last.payload[0] == CASC_NACK_ADDR,
              "应以 NACK(ADDR) 明确回绝（静默回绝会让现场只能猜）");

    /* ② `yours` 就是本机地址（0）→ 只确认角色，不改地址、不写记录 */
    fixture_reset();
    p = set_addr_payload(CASC_ADDR_MASTER, CASC_ADDR_MASTER, 100);
    peer_frame(1, CASC_ADDR_MASTER, CASC_T_SET_ADDR, 12, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_tx_last.type == CASC_T_ACK, "yours 等于本机地址时应回 ACK 确认");
    CHECK_MSG(s_self_addr == 0 && s_save_calls == 0 && s_reinit_calls == 0,
              "只确认角色时不该改身份、不该重装门面、不该写记录");

    /* ③ 自己的回声（半双工）：**连 ACK 都不能回** —— 回了会把发起方的等待槽
       当成"对方的答复"，于是发起方记下一个并不存在的确认 */
    fixture_reset();
    p = set_addr_payload(CASC_ADDR_MASTER, 2, 100);
    peer_frame(s_self_addr, CASC_ADDR_MASTER, CASC_T_SET_ADDR, 13, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_tx_last.len == 0, "回声不该有任何应答，却发了 %u 字节",
              (unsigned)s_tx_last.len);
    CHECK_MSG(s_self_addr == 0 && s_save_calls == 0, "回声不该改身份");

    /* ④ 本机是从卡 → 改成 yours 并**持久化**，然后作废陈旧状态、回 ACK */
    fixture_reset();
    s_self_addr = 1;                       /* 本机是从卡（addr 1） */
    s_state[0]  = SCREEN_CARD_ONLINE;
    s_state[1]  = SCREEN_CARD_ONLINE;
    p           = set_addr_payload(CASC_ADDR_MASTER, 2, 100);
    peer_frame(CASC_ADDR_MASTER, 1, CASC_T_SET_ADDR, 14, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 2, "从卡收到识别帧应改成 yours=2，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == 2,
              "改完必须**立刻写记录**（掉电不忘是这条的全部意义）");
    CHECK_MSG(s_reinit_calls == 1, "改完必须重装门面（渲染目标/持久化钩子要跟着换）");
    CHECK_MSG(s_tx_last.type == CASC_T_ACK, "改完应回 ACK，否则发起方以为没通知到");
    CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_MISSING &&
                  app_screen_card_state(1) == SCREEN_CARD_MISSING,
              "身份变了就要把「谁在线」全部作废 —— 那是按**旧地址**建立的");

#if BOARD_HAS_ADDR_DIP
    /* ⑤ 拨码板：`yours` 与拨码不符 → **拒绝**（拨码优先），且日志要说清是本板有拨码 */
    fixture_reset();
    s_has_dip    = true;
    s_dip_bit[0] = true; /* 拨码 = 1 */
    s_self_addr  = 1;
    p            = set_addr_payload(CASC_ADDR_MASTER, 2, 100);
    peer_frame(CASC_ADDR_MASTER, 1, CASC_T_SET_ADDR, 15, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1, "拨码板不许被改成 2（拨码优先），得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "被拒绝时不该写记录");
    CHECK_MSG(s_tx_last.type == CASC_T_NACK && s_tx_last.payload[0] == CASC_NACK_ADDR,
              "拨码冲突应以 NACK(ADDR) 回绝");
#endif
}

/** 按键认领：只投递请求 → 写记录 → 重装门面 → 作废状态 → 逐卡单播识别帧 */
static void case_claim_master_flow(void)
{
    TEST_BEGIN("按键认领：写记录、重装门面、作废状态、逐卡通知并等 ACK");

    fixture_reset();
    s_state[0] = SCREEN_CARD_ONLINE; /* 从卡在线，才会被通知 */
    s_state[1] = SCREEN_CARD_ONLINE;

    app_cascade_claim_master();
    CHECK_MSG(s_claim_req, "按键只投递请求（写 flash + 等 ACK 最长约 1s，必须由级联任务做）");
    CHECK_MSG(s_slave.set_addr_rx == 0, "投递请求时还不该发任何帧（动作在级联任务里）");

    const uint32_t t0 = osKernelGetTickCount();
    _claim_poll(); /* casc_task 里那一格 */
    CHECK_MSG(!s_claim_req, "取走请求后标志要清掉（否则每圈都认领一次）");

    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == CASC_ADDR_MASTER,
              "认领要先把自己写成主卡（addr=0）并落盘，得到 save=%d addr=%u", s_save_calls,
              (unsigned)s_last_saved_addr);
    /* **身份没变就不重装门面**：重装会清画布 + 清"画布被写过"的闩 ——
       那等于"按一下键，刚渲染的内容当场消失"（工厂测试第一次按键显示编码就是这么丢的） */
    CHECK_MSG(s_reinit_calls == 0,
              "本卡已经是 addr 0 时认领不该重装门面（重装会清掉刚画好的内容），得到 %d 次",
              s_reinit_calls);
    CHECK_MSG(s_self_addr == CASC_ADDR_MASTER, "认领后本机就是主卡");
    CHECK_MSG(s_my_claim != 0xFFFFFFFFU, "认领时刻要记下来（两张都自称主卡时靠它裁决）");

    CHECK_MSG(s_slave.set_addr_rx == 1, "应向布局里其余每张卡**逐卡单播**识别帧，得到 %d 条",
              s_slave.set_addr_rx);
    CHECK_MSG(s_slave.set_addr_payload.mine == CASC_ADDR_MASTER &&
                  s_slave.set_addr_payload.yours == 1,
              "识别帧应自称 mine=0、并让对端成为 1 号（得到 mine=%u yours=%u）",
              (unsigned)s_slave.set_addr_payload.mine,
              (unsigned)s_slave.set_addr_payload.yours);
    CHECK_MSG(casc_get_u32(s_slave.set_addr_payload.claim) == s_my_claim,
              "claim 必须是本卡认领那一刻的 tick（两卡对同一对数比较，结论才一致）");
    /* 对端的 ACK 走的是**收**路径（不进 ccb_send），所以看等待槽：匹配上 (seq,src)
       才说明"确实通知到了"，而不是"发出去了就算" */
    CHECK_MSG(s_ack.valid && s_ack.src == 1 && s_ack.sta == CASC_STA_ACK,
              "认领要等到对端的 ACK 并匹配上 (seq,src)（等不到就该重发/报出来）");

    /* 认领会把协议侧按旧身份建立的陈旧状态全部作废，并**立刻**重新枚举 */
    CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_MISSING, "认领后「谁在线」要全部作废");
    CHECK_MSG(s_reenum_at >= t0 && s_reenum_at <= osKernelGetTickCount(),
              "认领后应立刻重新枚举（否则新主卡要等下一个 10s 周期才被发现）");
}

/** 身份变了：在途轮次必须自己收尾（不许按旧身份继续发、也不许提交本地） */
static void case_round_aborts_on_identity_change(void)
{
    TEST_BEGIN("一轮中途身份变了 → 立即收尾：不重发、不提交本地、不计失败");

    fixture_reset();
    s_change_addr_after_rx = 5; /* 帧一发出去就"被改了身份"（按键认领/识别帧都可能是触发点） */

    CHECK_MSG(!_round_run(), "身份变了那一轮必须判失败（上电对齐据此重试）");
    CHECK_MSG(s_slave.image_rx == 1, "身份变了不该再重发（重发的 dst/矩形已经是错的），得到 %d 条",
              s_slave.image_rx);
    CHECK_MSG(s_commit_self_calls == 0,
              "身份变了不该提交本地 —— 那时画布已被重装清空，提交上去是把空画面推上屏");
    CHECK_MSG(s_fail_run[0] == 0,
              "这次失败**不算这张卡的**：算了的话，几轮认领下来会把一张好卡剔除");
}

/** `_identity_poke`：把协议侧按旧身份建立的陈旧状态全部作废 */
static void case_identity_poke(void)
{
    TEST_BEGIN("身份变了 → 作废陈旧状态并立刻重新枚举");

    fixture_reset();
    s_state[0]    = SCREEN_CARD_ONLINE;
    s_state[1]    = SCREEN_CARD_ONLINE;
    s_fail_run[0] = 3;
    s_fail_run[1] = 2;
    s_ack.valid   = true;
    s_force_round = true;

    const uint32_t t0 = osKernelGetTickCount();
    _identity_poke();

    CHECK_MSG(app_screen_card_state(0) == SCREEN_CARD_MISSING &&
                  app_screen_card_state(1) == SCREEN_CARD_MISSING,
              "各卡状态必须回 MISSING：新身份要先重新枚举才知道谁在");
    CHECK_MSG(s_fail_run[0] == 0 && s_fail_run[1] == 0, "旧的失败计数同样作废");
    CHECK_MSG(!s_force_round, "开轮请求不该跨身份继续（画布已重装）");
    CHECK_MSG(!s_ack.valid, "等应答槽要作废（那是旧身份那一轮的回应）");
    CHECK_MSG(s_reenum_at >= t0 && s_reenum_at <= osKernelGetTickCount(),
              "应把下一次枚举提前到**现在**，而不是等 10s 周期");
}

/** 开轮三闸：画布没写过不开 / **渲染途中不开** / 静默期没到不开
 *
 *  第三条是现场"切换下一个字之前闪一下"的根因：渲染是"测量趟 + 渲染趟"，
 *  清屏与文字之间画布是空的 —— 这一眼推下去，两块屏先黑一帧再出新字。
 *  闸住之后，一轮只在"内容定稿"之后才开。
 *
 *  **反向验证**：去掉 `_round_ready` 里 `if (app_render_busy()) return false;`
 *  那一行，本用例的"渲染途中"两条立刻红。 */
static void case_round_ready_gates(void)
{
    TEST_BEGIN("开轮三闸：画布没写过 / 渲染途中 / 静默期（force 也要过前两闸）");

    fixture_reset();

    /* ① 画布没被写过：一轮都不许开 */
    s_canvas_touched = false;
    s_settled        = true;
    s_render_busy    = false;
    CHECK_MSG(!_round_ready(), "画布没被写过时不许开轮（上电恢复的内容会被刷黑）");

    /* ② 画布写过 + 静默期到 → 开 */
    s_canvas_touched = true;
    s_settled        = true;
    CHECK_MSG(_round_ready(), "画布写过且静默期已过应开轮");

    /* ③ **渲染途中**：静默期到了也不许（这一眼画布上只有半张，甚至刚清空） */
    s_canvas_touched = true;
    s_settled        = true;
    s_render_busy    = true;
    CHECK_MSG(!_round_ready(), "渲染途中不许开轮 —— 推下去的就是那一帧「闪一下」的中间态");
    s_render_busy = false;

    /* ④ 静默期没到：内容还在变，攒着 */
    s_canvas_touched = true;
    s_settled        = false;
    CHECK_MSG(!_round_ready(), "静默期没到不许开轮");

    /* ⑤ `force_round`（有卡刚上线）**也要过前两闸**，而且请求不许被白白吃掉 */
    s_settled        = false;
    s_canvas_touched = false;
    s_force_round    = true;
    CHECK_MSG(!_round_ready() && s_force_round,
              "新上线的卡也要等画布被写过 —— 且这一眼的请求必须留着");
    s_canvas_touched = true;
    s_render_busy    = true;
    CHECK_MSG(!_round_ready() && s_force_round, "渲染途中 force_round 同样不许开");
    s_render_busy = false;
    CHECK_MSG(_round_ready() && !s_force_round, "闸都过了才开，并把 force 请求清掉");

    /* ⑥ 请求被清掉之后，下一下不再开（依赖画布内容，而不是依赖这个标志） */
    s_settled = false;
    CHECK_MSG(!_round_ready(), "force 用掉之后要靠静默期那条路");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m级联图传 · 主卡侧（本板单卡 %ux%u，一帧 %u 字节）\033[0m\n", (unsigned)CARD_W,
           (unsigned)CARD_H, (unsigned)(CASC_OVERHEAD + sizeof(casc_image_t) + CARD_BM));

    case_round_ok();
    case_rect_matches_addr();
    case_retransmit_whole_frame();
    case_silent_slave_master_still_commits();
    case_permanent_loss_gives_up();
    case_nack_gives_up();
    case_one_extract_per_card();
    case_evict_skip_recover();
    case_retrans_counted();
    case_id_resolve_sources();
    case_set_addr_branches();
    case_claim_master_flow();
    case_round_aborts_on_identity_change();
    case_round_ready_gates();
    case_identity_poke();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
