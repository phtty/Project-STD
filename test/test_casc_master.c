/**
 * @file    test_casc_master.c
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
 * 总线是假的：`app_ccb_send` 旁边坐着一个**假从卡**，它按协议收 IMAGE、组 ACK/NACK，再经
 * **真探针 + 真队列**喂回主卡 —— 所以走的是真的分帧、真的探针、真的等待循环。
 *
 * app_screen 是替身：抽带与落屏的**正确性**由 test_screen_layout.c 覆盖，
 * 这里只关心"主卡把哪一块发给了谁、发了几次"。边界就划在
 * `app_screen_extract` / `app_screen_commit_self` 上。
 */

#define BOARD_SCREEN_CANVAS 1
#define BOARD_CASC_ENABLED  1 /* 套件要测级联：单卡配置下也要把它编进来 */

/* 本夹具是一个 **1×2** 网格（见下面 s_card_table 的排布）—— 必须与它对齐：
   生产用 `CASC_CELL_COUNT`（合法地址集合 `[0, nx*ny)` 的大小）直接由这两个宏算出。
   `board.h` 里它们有 `#ifndef` 守卫：5006048 板本来就是这个形状（此处为 no-op），
   而默认的 3833024 是 1×1 —— 不钉住的话，用例里的 addr=1 会全变成越界值。 */
#define BOARD_CASC_COLS 1
#define BOARD_CASC_ROWS 2

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* write()/_exit()：看门狗（见 main 前的说明） */

/* 身份/记录/按键的桩要用到这些类型（本套件不测它们的行为）*/
#include "app_cfg_sched.h"
#include "dev_key.h"
#include "app_casc.h"
#include "app_screen.h"
#include "dev_display.h"
#include "ring_buffer.h"

/* ---- 本板的单卡几何（本套件按**最大那块卡**取）----
 * 5006048 是 224×50 → 28×50 = 1400；3833024 是 96×48 → 12×48 = 576
 * （该板 6 模组 2 卡不等宽：主卡 96×48、从卡 48×48，上限取大的那块）。
 *
 * `BOARD_CASC_BAND_MAX` 只是**编译期上限**（按最大卡留，允许有余量），
 * 位图长度按本夹具的 CARD_W×CARD_H 实算 —— 生产侧同样是按运行期矩形
 * 算 `app_screen_card_bm_len()`，不拿上限当长度。 */
#if BOARD_CASC_BAND_MAX >= 1400
#define CARD_W (224U)
#define CARD_H (50U)
#else
#define CARD_W (96U)
#define CARD_H (48U)
#endif
#define CARD_BM (((CARD_W + 7U) / 8U) * CARD_H)

static_assert(CARD_BM <= BOARD_CASC_BAND_MAX,
              "本板的单卡几何装不进 BOARD_CASC_BAND_MAX —— 新板要在这里补一行");

/* 一帧必须装得下整块位图 —— 这正是"一帧一轮"成立的前提，装不下就别谈删分片 */
static_assert(CASC_FRAME_MAX >= CASC_OVERHEAD + sizeof(app_casc_image_t) + CARD_BM,
              "本板位图一帧装不下");

/* ---- 本卡身份：**夹具可改**（身份用例要换 addr、换格） ----
 *
 *  主从判据与格↔地址的规则都照生产的口径写，免得夹具自己造出一套与生产不同的
 *  规则 —— 那样测出来的"对"与屏上的"对"是两回事。
 *  本夹具的排布是 `s_card_table = {格0: addr1, 格1: addr0}`（主卡在下）→ **主卡格 = 1**。 */
static uint8_t s_self_addr   = 0;
static uint8_t s_master_cell = 1;

uint8_t app_screen_index_of_addr(uint8_t a); /* 前置声明：定义在下面 */

bool app_screen_is_master(void)
{ return s_self_addr == CASC_ADDR_MASTER; }
uint8_t app_screen_self_addr(void)
{ return s_self_addr; }
void app_screen_set_addr(uint8_t a)
{ s_self_addr = a; }

uint8_t app_screen_master_cell(void)
{ return s_master_cell; }

uint8_t app_screen_addr_of_cell(uint8_t cell, uint8_t mc)
{
    if (cell == mc) return 0;
    return (uint8_t)(cell < mc ? cell + 1U : cell);
}
uint8_t app_screen_cell_of_addr(uint8_t addr, uint8_t mc)
{
    if (addr == 0) return mc;
    return (uint8_t)(addr <= mc ? addr - 1U : addr);
}

static int s_reinit_calls;
void app_screen_reinit_identity(void)
{ s_reinit_calls++; }

/** 与生产同语义：**一个都没变就什么都不做**（重装会清画布 + 清闩） */
void app_screen_apply_identity(uint8_t addr, uint8_t mc)
{
    if (addr == s_self_addr && mc == s_master_cell) return;
    s_self_addr   = addr;
    s_master_cell = mc;
    s_reinit_calls++;
}

/* ---- app_screen 替身 ---- */

/* **按现场的真实拓扑排**：上下拼，**下面那块是主卡**。
   于是 下标 0 是上面的从卡（addr 1）、下标 1 是下面的主卡（addr 0）——
   **addr 与下标正好相反**，这正是"拿地址当下标"最容易出错、也最该被测到的形态。
   （若按 addr==下标 排，那种错法在这套用例里完全测不出来。） */
#define SELF_IDX (1U) /* 本卡（主卡 addr 0）落在下标 1 */

static app_screen_card_t s_card_table[2] = {
    {.addr = 1, .color = DEV_DISPLAY_COLOR_RED, .x = 0, .y = 0, .w = CARD_W, .h = CARD_H},        /* 上：从卡 */
    {.addr = 0, .color = DEV_DISPLAY_COLOR_GREEN, .x = 0, .y = CARD_H, .w = CARD_W, .h = CARD_H}, /* 下：主卡 */
};
static const app_screen_layout_t s_screen_layout = {
    .cards = s_card_table,
    .count = 2,
    .rows  = CARD_W,
    .cols  = (uint16_t)(CARD_H * 2),
};

const app_screen_layout_t *app_screen_layout(void)
{ return &s_screen_layout; }
const app_screen_card_t *app_screen_card(uint8_t idx)
{ return (idx < 2) ? &s_card_table[idx] : nullptr; }
uint16_t app_screen_card_bm_len(uint8_t idx)
{ return (idx < 2) ? CARD_BM : 0; }
uint8_t app_screen_self_index(void)
{
    return app_screen_index_of_addr(s_self_addr); /* 身份一改，"我是哪一格"跟着走 */
}

/* ---- 卡片状态：**忠实实现**，不是空桩 ----
 *
 * P4 的策略（不在线就跳过、连续失败到阈值剔除、收到 PRESENT 就回到在线）全靠这几个
 * 函数与轮次配合。给空桩的话，那几条策略在这套用例里就一条也测不到。 */
static uint8_t s_state[2] = {APP_SCREEN_CARD_STATE_MISSING, APP_SCREEN_CARD_STATE_MISSING};
static uint16_t s_last_seq;
static int s_retrans_cnt;

app_screen_card_state_t app_screen_card_state(uint8_t i)
{
    return (i < 2) ? (app_screen_card_state_t)s_state[i] : APP_SCREEN_CARD_STATE_MISSING;
}
void app_screen_card_set_state(uint8_t i, app_screen_card_state_t st)
{
    if (i < 2) s_state[i] = (uint8_t)st;
}
uint8_t app_screen_index_of_addr(uint8_t a)
{
    for (uint8_t i = 0; i < 2; i++)
        if (s_card_table[i].addr == a) return i;
    return 0xFF;
}
void app_screen_note_round(uint16_t seq)
{ s_last_seq = seq; }
void app_screen_note_retrans(void)
{ s_retrans_cnt++; }

/* ---- 身份记录：**一小块存储器**（用例据此构造"有记录 / 无记录 / 读失败"） ----
 *
 *  桩成"永远读不到"就只能测出厂默认那一条；而身份的来源有三档、优先级才是被测的东西。 */
static bool s_rec_present;                                            /* 有没有记录 */
static uint8_t s_rec_addr, s_rec_src;                                 /* 记录里的内容 */
static uint8_t s_rec_mc                    = 1;                       /* 记录里的主卡格 */
static dev_cfg_record_state_t s_load_state = DEV_CFG_RECORD_STATE_OK; /* 用例可摆成 IO_ERR */
static uint16_t s_load_len = 4; /* 读出的负载长度（4 = 本工程的记录体）；用例可摆成"长度不符" */
static int s_load_calls;
static int s_save_calls;
static uint8_t s_last_saved_addr, s_last_saved_src, s_last_saved_mc;

uint8_t app_cfg_sched_register(const app_cfg_sched_desc_t *d)
{
    (void)d;
    return 0; /* 有效句柄：注册失败的话身份就永远不落盘，那是另一条用例 */
}
dev_cfg_record_state_t app_cfg_sched_load(uint8_t id, uint8_t *p, uint16_t c, uint16_t *l)
{
    (void)id;
    s_load_calls++;
    if (s_load_state != DEV_CFG_RECORD_STATE_OK) return s_load_state; /* IO_ERR：**必须**原样上报，不许被缓存 */
    if (!s_rec_present) return DEV_CFG_RECORD_STATE_EMPTY;
    if (c < 4) return DEV_CFG_RECORD_STATE_EMPTY;
    p[0] = s_rec_addr; /* 记录体 = {addr, src, master_cell, rsv} */
    p[1] = s_rec_src;
    p[2] = s_rec_mc;
    p[3] = 0;
    *l   = s_load_len; /* 长度由用例摆：≠4 时生产侧应判"记录不可用"并回落 */
    return DEV_CFG_RECORD_STATE_OK;
}
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *p, uint16_t n)
{
    (void)id;
    if (n >= 2) {
        s_save_calls++;
        s_last_saved_addr = p[0];
        s_last_saved_src  = p[1];
        s_last_saved_mc   = p[2];
    }
    return 0;
}

/* ---- 拨码：用例可摆出"本板有拨码、值是几"（只有 BOARD_HAS_ADDR_DIP=1 的板会读它） ---- */
static dev_key_t s_dip_key;
static bool s_has_dip;
static bool s_dip_bit[2];

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
void app_render_save(void)
{
}
bool app_render_peek_persist_req(void)
{ return false; }
uint8_t app_screen_output_color(uint8_t idx, uint8_t c)
{
    (void)idx;
    return c;
} /* 无颜色覆盖 */

/* ---- 开轮三闸的输入：用例逐个摆（`_round_ready` 就是靠这三个判的） ---- */
static bool s_canvas_touched_flag, s_settled, s_render_busy;

bool app_screen_canvas_touched(void)
{ return s_canvas_touched_flag; }
bool app_render_busy(void)
{ return s_render_busy; }
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
bool app_screen_commit_self(void)
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
void app_screen_set_brightness(uint8_t l)
{ (void)l; }
uint8_t app_screen_get_brightness(void)
{ return 3; }
bool app_screen_brightness_take_pending(uint8_t *l)
{
    (void)l;
    return false;
}
/* ---- 显示与总线 ---- */
#define DEV_W (48U)
#define DEV_H (16U)
static dev_display_t s_dev;
static uint8_t s_pixel_map[DEV_W * DEV_H];
dev_display_t *dev_display_get(void)
{ return &s_dev; }

/* ---- 被测：生产源码本体 ---- */
#include "../Application/Src/CASC/app_casc.c"

/* ================================================================
 *  假从卡 —— 坐在总线另一头，按协议收 IMAGE、组 ACK
 * ================================================================ */

static struct {
    /** 收到过几条 IMAGE（**重整帧重传靠它验证**：一帧一轮，重传就是整帧重发） */
    int image_rx;
    uint16_t seq;           /**< 最近一条 IMAGE 的轮次序号 */
    uint16_t bmp_len;       /**< 最近一条 IMAGE 声明的位图长度 */
    uint8_t stage[CARD_BM]; /**< 收到的位图（与主卡抽给**本卡**的那块比对） */
    bool applied;           /**< 有内容落下来过 */

    /* 注入 */
    bool drop_first; /**< **只丢首发**（模拟总线瞬时丢帧，重传能过） */
    bool no_apply;   /**< 收到了但落不下去（也不回 ACK）—— 持续干扰/卡忙 */
    bool silent;     /**< 连收都没收到（卡掉线）：一句话都不回 */
    bool nack;       /**< 明确回绝（模拟矩形与本卡切分表不符） */
    bool nack_addr;  /**< 拒绝识别帧（模拟"本卡有拨码、地址由拨码定"） */

    /* 识别帧：单播（逐卡）与广播各记一份 */
    uint8_t addr; /**< 本从卡的地址（默认 1；认领用例会把它设成旧主卡的 0） */
    int set_addr_rx;
    app_casc_set_addr_t set_addr_payload;
    int bcast_set_addr_rx;
    app_casc_set_addr_t bcast_payload;

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

/** @brief 相当于 app_dispatch_task 的那一圈：探针 → rb_read → 入队
 *
 *  真框架是"等通道通知 → 遍历该通道的协议 → 探针 → rb_read → osMessageQueuePut"。
 *  本用例没有通道线程，所以由假从卡在"回了一帧之后"手动跑这一圈。 */
static void dispatch_pending(void)
{
    static uint8_t buf[sizeof(app_dispatch_msg_t) + FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));
    app_dispatch_msg_t *const msg = (app_dispatch_msg_t *)buf;

    for (;;) {
        uint32_t tl              = 0;
        uint8_t aux              = 0;
        app_pcb_probe_state_t st = _casc_probe_frame(&s_casc_pcb, nullptr, nullptr, msg->data,
                                                     FRAME_DATA_MAX_LEN, &tl, &aux);
        if (st == APP_PCB_PROBE_STATE_FAKE) {
            rb_skip(&s_rb, 1, nullptr);
            continue;
        }
        if (st != APP_PCB_PROBE_STATE_READY) return;

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
    uint8_t f[CASC_FRAME_MAX];
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(f, 0, len);
    f[0] = CASC_SOF0;
    f[1] = CASC_SOF1;
    f[2] = (uint8_t)((CASC_PROTO_VER << 6) | type);
    f[3] = dst;
    f[4] = src;
    _casc_put_u16(f + 5, seq);
    _casc_put_u16(f + 9, len);
    if (plen) memcpy(f + 11, payload, plen);
    _casc_put_u32(f + len - 4U, pl_crc32_calc(pl_crc_get_handle(), f + 2, len - 6U));

    rb_write(&s_rb, f, len, nullptr);
    dispatch_pending();
}

/** @brief 假从卡回一帧给主卡（dst=0, src=1） */
static void slave_reply(uint8_t type, uint16_t seq, const void *payload, uint16_t plen)
{
    peer_frame(s_slave.addr, CASC_ADDR_MASTER, type, seq, payload, plen);
}

/** @brief 组一条识别帧的载荷（`claim` 由用例另外填：它决定回声过滤与让位裁决） */
static app_casc_set_addr_t set_addr_payload(uint8_t mine, uint8_t yours, uint8_t master_cell)
{
    app_casc_set_addr_t p = {.mine = mine, .yours = yours, .master_cell = master_cell};
    return p;
}

static void slave_send_present(void)
{
    app_casc_present_t p;
    memset(&p, 0, sizeof(p));
    p.addr = 1;
    _casc_put_u16(p.w, CARD_W);
    _casc_put_u16(p.h, CARD_H);
    p.bright    = 3;
    p.proto_ver = CASC_PROTO_VER;
    slave_reply(APP_CASC_TYPE_PRESENT, 1, &p, sizeof(p));
}

/** @brief 回一条 ACK（**无载荷**：主卡只按 (seq, src) 认它） */
static void slave_send_ack(uint16_t seq)
{ slave_reply(APP_CASC_TYPE_ACK, seq, nullptr, 0); }

static uint8_t s_tx_count; /**< 主卡一共发了多少帧 */

/** 本卡最近发出去的一帧（身份用例要断言"回了什么"：ACK / NACK(ADDR)） */
static struct {
    uint8_t type, dst, src;
    uint16_t seq, len;
    uint8_t payload[CASC_FRAME_MAX];
} s_tx_last;

/** 假从卡收到 IMAGE 之后把本卡身份改成这个值（-1 = 不改）—— 用来构造
 *  "一轮中途身份变了"，那是 `s_round_me` 那条收尾逻辑的唯一触发条件。 */
static int s_change_addr_after_rx = -1;

/** @brief 假从卡：收主卡发来的帧 */
static void slave_on_master_frame(const uint8_t *d, uint16_t l)
{
    const uint8_t type = CASC_TYPE_OF(d[2]);
    const uint16_t seq = _casc_get_u16(d + 5);

    /* 总线是共享的，但**不是发给本卡的帧一律不理** —— 与真从卡一样。
       少了这条，"主卡把帧发给了自己（dst=0）"这种错会被当成正常收到。 */
    if (d[3] != s_slave.addr && d[3] != CASC_ADDR_BCAST) return; /* 只收发给自己的与广播 */

    /* ---- 识别帧：**单播（dst=本卡地址）与广播（dst=0xFF）都收** ----
       广播那条是"两张都自称主卡"时唯一到得了对方的路（逐卡单播只发给表里的地址）。 */
    if (type == APP_CASC_TYPE_SET_ADDR) {
        const bool bcast = (d[3] == CASC_ADDR_BCAST);
        if (bcast)
            s_slave.bcast_set_addr_rx++;
        else
            s_slave.set_addr_rx++;

        uint8_t *dstbuf = bcast ? (uint8_t *)&s_slave.bcast_payload
                                : (uint8_t *)&s_slave.set_addr_payload;
        memset(dstbuf, 0, sizeof(s_slave.set_addr_payload));
        const uint16_t plen = (uint16_t)(l - CASC_OVERHEAD);
        memcpy(dstbuf, d + sizeof(app_casc_hdr_t),
               plen < sizeof(s_slave.set_addr_payload) ? plen : sizeof(s_slave.set_addr_payload));
        if (bcast) return; /* 广播不需要本从卡应答（发起方按"发出去了"计数） */

        /* **真从卡会先改自己的身份、再用新地址应答**（`_cmd_set_addr` 里就是这个顺序），
           夹具照做 —— 否则回带的 src 还是旧地址，而主卡等的正是新地址。 */
        if (!s_slave.nack_addr) s_slave.addr = s_slave.set_addr_payload.yours;

        if (s_slave.nack_addr) {
            const app_casc_nack_t n = {.err = APP_CASC_NACK_ADDR};
            slave_reply(APP_CASC_TYPE_NACK, seq, &n, sizeof(n));
        } else {
            slave_send_ack(seq);
        }
        return;
    }

    if (type != APP_CASC_TYPE_IMAGE) return;

    s_slave.image_rx++;
    s_slave.seq            = seq;
    s_slave.last_frame_len = l;

    /* 一轮中途换身份（真正的触发点在"发帧"这一瞬，也就是 _round_one_card 还在跑的时候） */
    if (s_change_addr_after_rx >= 0) s_self_addr = (uint8_t)s_change_addr_after_rx;

    const app_casc_image_t *p = (const app_casc_image_t *)(d + sizeof(app_casc_hdr_t));
    s_slave.bmp_len           = _casc_get_u16(p->bmp_len);
    memcpy(s_slave.last_x, p->x, 2);
    memcpy(s_slave.last_y, p->y, 2);
    memcpy(s_slave.last_w, p->w, 2);
    memcpy(s_slave.last_h, p->h, 2);

    if (s_slave.nack) { /* 矩形与本卡切分表不符：明确拒绝，重发没用 */
        const app_casc_nack_t n = {.err = APP_CASC_NACK_GEOM};
        slave_reply(APP_CASC_TYPE_NACK, seq, &n, sizeof(n));
        return;
    }
    if (s_slave.silent) return;                              /* 卡掉线：连收都没收到 */
    if (s_slave.no_apply) return;                            /* 收到了但落不下去，也就不该回 ACK */
    if (s_slave.drop_first && s_slave.image_rx == 1) return; /* 只丢首发 */

    /* 校验帧长自洽（真从卡也会这么做），然后收下整幅 */
    if (l != (uint16_t)(CASC_OVERHEAD + sizeof(app_casc_image_t) + s_slave.bmp_len)) return;
    if (s_slave.bmp_len != CARD_BM) return;

    memcpy(s_slave.stage, p->bitmap, s_slave.bmp_len);
    s_slave.applied = true;
    slave_send_ack(seq);
}

int32_t app_ccb_send(app_ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    s_tx_count++;
    if (l >= CASC_OVERHEAD) {
        memset(&s_tx_last, 0, sizeof(s_tx_last));
        s_tx_last.type = CASC_TYPE_OF(d[2]);
        s_tx_last.dst  = d[3];
        s_tx_last.src  = d[4];
        s_tx_last.seq  = _casc_get_u16(d + 5);
        s_tx_last.len  = l;
        if (l - CASC_OVERHEAD <= sizeof(s_tx_last.payload))
            memcpy(s_tx_last.payload, d + sizeof(app_casc_hdr_t), (size_t)(l - CASC_OVERHEAD));
        slave_on_master_frame(d, l);
    }
    return (int32_t)l;
}
app_ccb_t *app_rs485_ccb(void)
{ return nullptr; }
void app_dispatch_bind(app_pcb_t *p, app_ccb_t *c)
{
    (void)p;
    (void)c;
}
void pl_task_new_stub(void)
{
}

/* ================================================================
 *  夹具与断言
 * ================================================================ */

static void fixture_reset(void)
{
    s_rb.read_index  = 0;
    s_rb.write_index = 0;
    s_casc_pcb.rb    = &s_rb;

    /* **两处都要指到同一个队列**：假从卡往 pcb.queue 里投（那是框架投递的目标），
       而主卡的排空读的是 s_casc_queue（`_casc_init` 里那一份）——
       本用例没跑 initcall，所以得自己把两者接上。只接一处的话 ACK 进了没人读的
       队列，表现是"每张卡都超时"，与"卡掉线"完全一样。 */
    s_casc_queue     = osMessageQueueNew(2, CASC_MSG_SIZE, nullptr);
    s_casc_pcb.queue = s_casc_queue;

    s_tx_count          = 0;
    s_extract_calls     = 0;
    s_commit_self_calls = 0;
    s_ack_state.valid   = false;

    /* 身份/记录/拨码：一律回出厂态（无记录、无拨码、主卡） */
    s_self_addr            = CASC_ADDR_MASTER;
    s_master_cell          = 1; /* 夹具排布：主卡在下 */
    s_reinit_calls         = 0;
    s_rec_present          = false;
    s_rec_addr             = 0;
    s_rec_src              = 0;
    s_rec_mc               = 1;
    s_last_saved_mc        = 0xFF;
    s_load_state           = DEV_CFG_RECORD_STATE_OK;
    s_load_len             = 4;
    s_load_calls           = 0;
    s_save_calls           = 0;
    s_last_saved_addr      = 0xFF;
    s_last_saved_src       = 0xFF;
    s_has_dip              = false;
    s_dip_bit[0]           = false;
    s_dip_bit[1]           = false;
    s_change_addr_after_rx = -1;
    s_my_claim             = 0xFFFFFFFFU;
    s_canvas_touched_flag  = false;
    s_settled              = false;
    s_render_busy          = false;
    s_claim_req_flag       = false;
    memset(&s_tx_last, 0, sizeof(s_tx_last));

    /* 身份记录的**注册**在生产里是 initcall（host 上不跑），不补这一下 `s_id_cfg`
       就一直是 0xFF —— 那样所有落盘路径都被静默跳过，用例测的就不是它要测的东西了。 */
    _casc_id_register();

    /* 默认：从卡在线。**不在线的卡会被轮次直接跳过**，所以每条用例都得先把它置在线，
       否则测的就不是它本来要测的东西了。 */
    s_state[0]         = APP_SCREEN_CARD_STATE_ONLINE;
    s_state[1]         = APP_SCREEN_CARD_STATE_ONLINE;
    s_last_seq         = 0;
    s_retrans_cnt      = 0;
    s_force_round_flag = false;
    /* 失败计数是 app_casc.c 的文件级静态，**跨用例会累积** —— 不重置的话
       几条"让从卡不回 ACK"的用例合起来就把卡剔除了，后面的用例全跟着变。 */
    for (uint8_t k = 0; k < APP_SCREEN_CARD_MAX; k++) s_fail_run[k] = 0;

    s_dev.screen_rows = DEV_W;
    s_dev.screen_cols = DEV_H;
    s_dev.pixel_map   = s_pixel_map;

    slave_reset();
    s_slave.addr = 1; /* 默认：对端是从卡（addr 1）；认领用例会改它 */
}

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                              \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
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

/** @brief 正常一轮：整块位图一条帧到齐、从卡收下并应答、主卡提交本地
 *
 *  任何一环少一次，现场就是"某块屏不更新"——而两侧都没有日志会指出是哪一步。 */
static void case_round_ok(void)
{
    TEST_BEGIN("正常一轮：一条 IMAGE 装下整块位图，从卡收下、主卡提交本地");

    fixture_reset();
    /* 返回值是给"上电对齐"用的：全部从卡都完成才算成 */
    CHECK_MSG(_round_run(), "全部从卡完成时应返回 true（上电对齐据此决定还要不要再试）");

    CHECK_MSG(s_slave.image_rx == 1, "从卡应收到 1 条 IMAGE，得到 %d", s_slave.image_rx);
    CHECK_MSG(s_slave.bmp_len == CARD_BM, "从卡理解的位图长度应为 %u，得到 %u",
              (unsigned)CARD_BM, (unsigned)s_slave.bmp_len);
    CHECK_MSG(s_slave.last_frame_len == (uint16_t)(CASC_OVERHEAD + sizeof(app_casc_image_t) + CARD_BM),
              "整帧长度应为 %u，得到 %u",
              (unsigned)(CASC_OVERHEAD + sizeof(app_casc_image_t) + CARD_BM),
              (unsigned)s_slave.last_frame_len);
    check_content_is_card(0, "正常一轮");

    CHECK_MSG(s_commit_self_calls == 1, "主卡应提交本地一次，得到 %d", s_commit_self_calls);
}

/** @brief 发出去的那帧必须带**那张卡自己**的矩形 —— 地址与下标不许错配
 *
 *  错配时 CRC、长度、几何校验**全部通过**，代价是两块屏的内容悄悄对调。 */
static void case_rect_matches_addr(void)
{
    TEST_BEGIN("IMAGE 带的是该卡自己的矩形（地址与下标不许错配）");

    fixture_reset();
    _round_run();

    /* 发给 addr 1 的必须是**它自己**那块（下标 0 = 上半屏 y=0），
       而不是主卡那块（下标 SELF_IDX = 下半屏 y=CARD_H）。 */
    const uint16_t y = _casc_get_u16(s_slave.last_y);
    CHECK_MSG(y == 0, "发给 addr 1 的矩形 y 应为 0（上半屏），得到 %u（=%u 说明把主卡那块发过去了）",
              (unsigned)y, (unsigned)CARD_H);
    CHECK_MSG(_casc_get_u16(s_slave.last_w) == CARD_W && _casc_get_u16(s_slave.last_h) == CARD_H,
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
    CHECK_MSG(s_card_table[0].addr != 0 && s_card_table[SELF_IDX].addr == 0,
              "本夹具必须让 addr 与下标**不同序**，否则这条用例没有分辨力");
    CHECK_MSG(s_card_table[0].y != s_card_table[SELF_IDX].y,
              "本夹具两张卡的矩形必须**不同位**，否则「发错卡」测不出来");

    /* 本卡（addr 0）那块**不该**被下发 —— 它由本地提交处理 */
    CHECK_MSG(s_extract_calls == 1, "只应抽取从卡那一块（本卡那块走 commit_self），抽了 %d 次",
              s_extract_calls);
}

/** @brief 丢一条 → 整帧重发（一帧一轮之后没有"只补某一片"这种粒度），补上后内容必须正确 */
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

/** @brief 从卡始终不回 ACK：主卡**不许冻结**（整块屏跟着一起停是最坏的失效形态），
 *         本地照常更新，且重试次数必须封顶 */
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

/** @brief 一直落不下去：重试封顶后放弃该卡，主卡自己照常更新
 *
 *  一张坏卡拖住整轮，表现就是"级联特别慢"，而两块屏的内容其实都是对的。 */
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

/** @brief 从卡回 NACK（几何不符）：立刻放弃该卡，**不做无用的重传**
 *
 *  重发还是不符，而每次白白占用一整帧的总线时间。 */
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

/** @brief 整轮不许重复抽取：每张从卡一次（重发用的是已经抽好的那帧）—— 多抽一次就多读一整块屏 */
static void case_one_extract_per_card(void)
{
    TEST_BEGIN("每张从卡只抽一次（重发用的是已经组好的那帧）");

    fixture_reset();
    s_slave.drop_first = true;
    (void)_round_run();

    CHECK_MSG(s_extract_calls == 1, "一次抽取 + 一次整帧重发也只该抽 1 次，抽了 %d 次",
              s_extract_calls);
}

/** @brief 不在线的卡一帧都不发（剔除的全部收益）；连续失败到阈值才剔除（剔太早会来回抖）；
 *         收到 PRESENT 立刻复在线并要求马上开一轮（否则它屏上还是掉线前那幅旧的） */
static void case_evict_skip_recover(void)
{
    TEST_BEGIN("跳过不在线 / 连续失败才剔除 / 收到 PRESENT 立刻复在线");

    /* ---- 从未应答过（MISSING）：一帧都不发 ---- */
    fixture_reset();
    s_state[0] = APP_SCREEN_CARD_STATE_MISSING;
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
        CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_ONLINE,
                  "第 %u 轮后不该剔除 —— 单轮失败多半是偶发冲突，下一轮自己就好，"
                  "剔了反而要等下一次枚举才叫得回来",
                  (unsigned)r);
    }
    (void)_round_run();
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_OFFLINE, "连续 %u 轮未完成应剔除",
              (unsigned)CASC_FAIL_RUN_MAX);

    /* ---- 被剔除的卡：**一帧都不再发**（这是剔除的全部收益）---- */
    const uint8_t before = s_tx_count;
    (void)_round_run();
    CHECK_MSG(s_tx_count == before, "被剔除的卡不该再收到任何帧，却发了 %u 帧",
              (unsigned)(s_tx_count - before));
    CHECK_MSG(s_commit_self_calls >= 1, "剔除一张卡不该拖住主卡自己的更新");

    /* ---- 它回来了：PRESENT → 立刻复在线，并要求马上开一轮 ---- */
    s_state[0]         = APP_SCREEN_CARD_STATE_OFFLINE;
    s_force_round_flag = false;
    slave_send_present();
    _casc_drain();
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_ONLINE, "收到 PRESENT 应复在线");
    CHECK_MSG(s_force_round_flag,
              "刚上线的卡应请求**立刻**开一轮 —— 它屏上还是掉线前那幅旧的，"
              "等下一次内容更新可能要几分钟");
}

/** @brief 整帧重传会计数 —— 状态快照要用它（"总线是不是在反复重传"只有这个数字能看出来） */
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

/** @brief 身份三档来源的优先级：拨码 > 记录 > 板级默认
 *
 *  读错只是两块屏的角色不对（画面照出），不会有任何一处报错；越界记录不拒、
 *  IO_ERR 被缓存这两条则会让身份永远停在错值上。 */
static void case_id_resolve_sources(void)
{
    TEST_BEGIN("身份来源：拨码 > 记录 > 板级默认（出厂态 = 主卡）");

    /* ① 出厂态：无记录、无拨码 → 板级默认（= 主卡）。
       这一档通常**与本机现状一致**（夹具的初值就是出厂态）→ 不该重装门面：
       重装会清画布与"画布被写过"的闩，那正是"按一下键、屏上内容消失"。 */
    fixture_reset();
    /* 先把本机摆成"出厂态"，才能验证"没变就不重装"这一条 —— 夹具的主卡格默认是 1
       （与它自己的卡表一致），而 3833024 板级配的 MASTER_CELL 是 0 */
    s_self_addr   = (uint8_t)BOARD_CASC_ADDR;
    s_master_cell = (uint8_t)BOARD_CASC_MASTER_CELL;
    _casc_id_boot();
    CHECK_MSG(s_self_addr == (uint8_t)BOARD_CASC_ADDR,
              "出厂态应取板级默认 %u，得到 %u", (unsigned)BOARD_CASC_ADDR,
              (unsigned)s_self_addr);
    CHECK_MSG(s_reinit_calls == 0,
              "身份与本机现状一致时不该重装门面（会清掉屏上内容），得到 %d 次", s_reinit_calls);

    /* ①b 本机现状与解析结果不同 → **必须**重装（这才是"换身份"那条路） */
    fixture_reset();
    s_self_addr = 1; /* 摆成与出厂默认不同的值 */
    _casc_id_boot();
    CHECK_MSG(s_self_addr == (uint8_t)BOARD_CASC_ADDR && s_reinit_calls == 1,
              "身份真的变了就要重装门面一次，得到 %d 次", s_reinit_calls);

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
    CHECK_MSG(s_self_addr == (uint8_t)BOARD_CASC_ADDR,
              "越界记录必须被拒、回落到默认 %u，得到 %u", (unsigned)BOARD_CASC_ADDR,
              (unsigned)s_self_addr);

    /* ④ 读失败（IO_ERR）**不许被缓存**：每次上电/每次解析都要去问一次，
       否则一次瞬时读失败会让身份永远停在默认值上 */
    fixture_reset();
    s_load_state = DEV_CFG_RECORD_STATE_IO_ERR;
    _casc_id_boot();
    _casc_id_boot();
    CHECK_MSG(s_load_calls == 2, "IO_ERR 不该被缓存：两次解析应两次都去读，得到 %d 次",
              s_load_calls);

#if BOARD_HAS_ADDR_DIP
    /* ⑤ 有拨码 → **拨码优先**，且不落盘（现场拨一下即生效，不需要工具） */
    fixture_reset();
    s_has_dip     = true;
    s_dip_bit[0]  = true; /* DIP1 = bit0，ON = 低 = 1 */
    s_dip_bit[1]  = false;
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

/** @brief 身份记录**不可用**时的读侧回落：状态/长度/地址越界 → 回落板级默认（两板 = 主卡），src=0
 *
 *  与"根本没有记录"（EMPTY，出厂态的正常一档）分开：那种情况**不出声**，而"槽已注册、
 *  读出来却不可用"会打一条 `[casc·id] 身份记录不可用 …` 的错误日志（给现场的诊断输出）。
 *  **日志本身不写断言**（日志没有稳定的可断言通道；断的是"回落后的行为"，日志有无靠
 *  代码评审保证）。
 *
 *  回落成 `BOARD_CASC_ADDR`（两板都是 0 = **主卡**）是**有意保留**的处置（用户口径：
 *  "不能停用；默认为主卡这个行为是可以的；报错可以有"）：记录坏掉的从卡上电后**会先当
 *  主卡**（可能两张主卡、一半屏停更），靠那条日志现场重新编址。
 *  这是**现状哨兵**：若将来把处置改成别的（停用 / 拒绝启动 / 按切分表收紧读侧），
 *  这些期望要一并改。 */
static void case_id_resolve_bad_record(void)
{
    TEST_BEGIN("身份记录不可用（状态/长度/地址越界）→ 回落板级默认（两板 = 主卡），src=0");

    uint8_t src  = 0xEE;
    uint8_t mc   = 0xEE;
    uint8_t addr = 0;

    /* ① 状态不对（INVALID：name/version/len/CRC 任一不符）→ 回落 */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 1;
    s_load_state  = DEV_CFG_RECORD_STATE_INVALID;
    addr = _id_resolve(&src, &mc);
    CHECK_MSG(addr == (uint8_t)BOARD_CASC_ADDR && src == 0,
              "记录 INVALID 应回落板级默认 %u（src=0），得到 addr=%u src=%u",
              (unsigned)BOARD_CASC_ADDR, (unsigned)addr, (unsigned)src);
    CHECK_MSG(mc == (uint8_t)BOARD_CASC_MASTER_CELL, "回落时主卡格取板级默认 %u，得到 %u",
              (unsigned)BOARD_CASC_MASTER_CELL, (unsigned)mc);

    /* ② 长度不对（load 报 OK，但负载长度 ≠ 本工程的记录体）→ 回落 */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 1;
    s_load_len    = 7; /* 本工程的记录体是 4 字节 */
    addr = _id_resolve(&src, &mc);
    CHECK_MSG(addr == (uint8_t)BOARD_CASC_ADDR && src == 0,
              "长度不符应回落板级默认 %u（src=0），得到 addr=%u src=%u", (unsigned)BOARD_CASC_ADDR,
              (unsigned)addr, (unsigned)src);
    CHECK_MSG(mc == (uint8_t)BOARD_CASC_MASTER_CELL, "回落时主卡格取板级默认 %u，得到 %u",
              (unsigned)BOARD_CASC_MASTER_CELL, (unsigned)mc);

    /* ③ 记录里的地址越界（> 0x1F 的地址根本不存在）→ 回落成主卡 */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 0x20; /* 刚越过读侧容错的上界 0x1F */
    addr = _id_resolve(&src, &mc);
    CHECK_MSG(addr == (uint8_t)BOARD_CASC_ADDR && src == 0,
              "越界地址（0x20）应回落成主卡（板级默认 %u），得到 addr=%u src=%u",
              (unsigned)BOARD_CASC_ADDR, (unsigned)addr, (unsigned)src);
    CHECK_MSG(mc == (uint8_t)BOARD_CASC_MASTER_CELL, "回落时主卡格取板级默认 %u，得到 %u",
              (unsigned)BOARD_CASC_MASTER_CELL, (unsigned)mc);

    /* ④ 对照组：有效记录 → 取记录里的地址与主卡格（**不回落**） */
    fixture_reset();
    s_rec_present = true;
    s_rec_addr    = 1;
    s_rec_src     = 3; /* 这条记录最初是谁写的，与"这次从哪读"无关 */
    s_rec_mc      = (uint8_t)(BOARD_CASC_MASTER_CELL == 0U ? 1U : 0U); /* 与板级默认不同的格 */
    addr = _id_resolve(&src, &mc);
    CHECK_MSG(addr == 1 && src == 1, "有效记录应取记录里的地址 1（src=1），得到 addr=%u src=%u",
              (unsigned)addr, (unsigned)src);
    CHECK_MSG(mc == s_rec_mc, "主卡格要取自记录（%u；板级默认是 %u），得到 %u",
              (unsigned)s_rec_mc, (unsigned)BOARD_CASC_MASTER_CELL, (unsigned)mc);
}

/** @brief 不该采纳的识别帧：非主卡发起（NACK）、自己的回声（静默）、拨码冲突（NACK）
 *
 *  三条都要求**零副作用**（身份、记录、门面一个都不许动）：
 *   · 非主卡那条守住"整个装置不会没有主卡" —— 从卡按键不能把唯一的主卡降级；
 *   · 回声那条若回了 ACK，会把发起方自己的等待槽当成"对方的答复"；
 *   · 拨码冲突那条守住"地址以拨码为准"（现场拨一下即生效，不需要工具）。 */
static void case_set_addr_rejected(void)
{
    TEST_BEGIN("识别帧·不该采纳的：非主卡拒绝 / 回声丢弃 / 拨码冲突拒绝");

    /* ① `mine != 0`：对方不是主卡 → 拒绝，本机身份不动。
       这一条守住"整个装置不会没有主卡"：从卡按键不能把唯一的主卡降级。 */
    fixture_reset();
    app_casc_set_addr_t p = set_addr_payload(1, 2, 100);
    peer_frame(1, CASC_ADDR_MASTER, APP_CASC_TYPE_SET_ADDR, 11, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 0, "非主卡的识别帧不该改本机身份，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "被拒绝的识别帧不该写记录");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "应以 NACK(ADDR) 明确回绝（静默回绝会让现场只能猜）");

    /* ③ 自己的回声：地址与自己相同**且 claim 也是自己那一刻** → 丢弃，连 ACK 都不回
       （回了会把发起方的等待槽当成"对方的答复"）。 */
    fixture_reset();
    s_my_claim = 100;
    p          = set_addr_payload(CASC_ADDR_MASTER, 0xFF, 1);
    _casc_put_u32(p.claim, s_my_claim);
    peer_frame(s_self_addr, CASC_ADDR_MASTER, APP_CASC_TYPE_SET_ADDR, 13, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_tx_last.len == 0, "回声不该有任何应答，却发了 %u 字节", (unsigned)s_tx_last.len);
    CHECK_MSG(s_self_addr == 0 && s_save_calls == 0, "回声不该改身份");

#if BOARD_HAS_ADDR_DIP
    /* ⑧ 拨码板：`yours` 与拨码不符 → **拒绝**（拨码优先）。
       `yours` 必须是**范围内**的冲突值：1×2 网格上 yours=2 会先被"超出网格"的门禁
       拒掉，那样测到的就不是"拨码优先"这条理由了（两条都回 NACK(ADDR)，现场难分）。 */
    fixture_reset();
    s_has_dip    = true;
    s_dip_bit[0] = false;             /* 拨码 = 0 */
    s_self_addr  = CASC_ADDR_MASTER;  /* 本机与拨码一致 */
    p            = set_addr_payload(CASC_ADDR_MASTER, 1, 1); /* 1 = 范围内、且与拨码冲突 */
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_MASTER, APP_CASC_TYPE_SET_ADDR, 15, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == CASC_ADDR_MASTER, "拨码板不许被改成 1（拨码=0 优先），得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "被拒绝时不该写记录");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "拨码冲突应以 NACK(ADDR) 回绝");
#endif
}

/** @brief 该采纳的识别帧：同址确认也落记录 / 单播改址 / 让位 / claim 更晚不让位 / 主卡格变了重算
 *
 *  共同点是"**身份、记录、门面三者必须一起动**"：只改内存不落盘（掉电忘）或
 *  只落盘不改内存（要重启才生效）在现场都是"时好时坏"。
 *  ⑦ 是"两块屏内容对调"那条的守门用例：**格不动、地址按新表重算**。 */
static void case_set_addr_applied(void)
{
    TEST_BEGIN("识别帧·该采纳的：同址确认落记录 / 单播改址 / 让位 / 不让位 / 按新格重算");

    /* ② 同址确认（`yours` 就是本机地址）→ 回 ACK，**而且要把身份写进记录**。
       从前这条路不写记录，于是从卡的地址一直靠出厂默认撑着 —— 默认值一改、
       或换一块板，它就跑到别的格上去了。 */
    fixture_reset();
    s_self_addr = 1; /* 本机是从卡，与记录里那条一致 */
    app_casc_set_addr_t p = set_addr_payload(CASC_ADDR_MASTER, 1, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 12, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_ACK, "同址确认应回 ACK");
    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == 1 && s_last_saved_mc == 1,
              "同址确认也要落记录（addr=1 主卡格=1），得到 save=%d addr=%u mc=%u", s_save_calls,
              (unsigned)s_last_saved_addr, (unsigned)s_last_saved_mc);
    CHECK_MSG(s_self_addr == 1 && s_reinit_calls == 0,
              "身份没变就不该重装门面（重装会清掉屏上刚画好的内容）");

    /* ④ **生产里真正单播出去的那条**（`_claim_notify_peers` 逐卡发的那条）：
       对方按键成了新主卡（新主卡格 = 0），本卡是旧主卡、在格 1 —— 新表里本卡是
       addr 1。本卡要接受"把主卡位让出去"：地址 0→1、主卡格 1→0，**地址与格一起走**。
       （旧用例这里用 yours=2 当"单播改址"，而它正是 1×2 网格上的越界值：范围门禁
       落地后那条会被 NACK —— 换成本条，测的还是同一条单播路，且是真帧。） */
    fixture_reset();
    s_self_addr = 0; /* 旧身份：主卡（格 1 那块） */
    s_state[0]  = APP_SCREEN_CARD_STATE_ONLINE;
    s_state[1]  = APP_SCREEN_CARD_STATE_ONLINE;
    p           = set_addr_payload(CASC_ADDR_MASTER, 1, 0); /* 新表：主卡格 0、本卡 addr 1 */
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_MASTER, APP_CASC_TYPE_SET_ADDR, 14, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1, "让出主卡位后本卡应是 addr 1，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_master_cell == 0, "主卡格要跟新表一起走（1→0），得到 %u", (unsigned)s_master_cell);
    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == 1 && s_last_saved_mc == 0,
              "改完必须**立刻写记录**（地址 + 主卡格），掉电不忘是这条的全部意义");
    CHECK_MSG(s_reinit_calls == 1, "改完必须重装门面（渲染目标/持久化钩子要跟着换）");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_ACK, "改完应回 ACK，否则发起方以为没通知到");
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_MISSING &&
                  app_screen_card_state(1) == APP_SCREEN_CARD_STATE_MISSING,
              "身份变了就要把「谁在线」全部作废 —— 那是按**旧地址**建立的");

    /* ⑤ **两张卡都自称主卡**（F1，让位那条路）：对方广播一条 `yours=0xFF` 的识别帧，
       本卡也认为自己在主卡格 → 让到**另一格**去。
       从前这一条根本走不到：发起方的单播只发给表里的从卡地址（对方在 addr 0 上收不到），
       而唯一的补救（"再按一次"）发的还是同一条帧 —— 现场就是**两张主卡卡死**。 */
    fixture_reset();
    s_my_claim = 0xFFFFFFFFU;                                 /* 本卡从没按过 → 一定让位 */
    p          = set_addr_payload(CASC_ADDR_MASTER, 0xFF, 1); /* 主卡格 = 1（本卡也这么认为） */
    _casc_put_u32(p.claim, 200);
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_BCAST, APP_CASC_TYPE_SET_ADDR, 16, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1, "两张都自称主卡时本卡应让到另一格（addr=1），得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_ACK, "让位后要回 ACK");
    CHECK_MSG(s_save_calls == 1, "让位要落记录（不然重启又变回两张主卡）");

    /* ⑥ 本卡按得**更晚**时不让位：claim 比较（更晚者胜 = 最近按的那张卡作数） */
    fixture_reset();
    s_my_claim = 300; /* 本卡在 300ms 认领过 */
    p          = set_addr_payload(CASC_ADDR_MASTER, 0xFF, 1);
    _casc_put_u32(p.claim, 200); /* 对方更早 */
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_BCAST, APP_CASC_TYPE_SET_ADDR, 17, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 0, "本卡按得更晚就不该让位，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK, "应明确回绝（静默会让对方一直等）");

    /* ⑦ **主卡格变了**（F3 的核心）：本卡是旧主卡（addr 0 = 格 1），对方按了键之后
       广播"主卡格 = 0" → 本卡**格不动**（还在格 1），地址按新表重算成 1。
       现场那半幅对调就是这里没做：地址跟着身份走了，格没走。 */
    fixture_reset();
    s_self_addr = 0;                                           /* 旧表：主卡格 1 → addr 0 = 格 1 */
    p           = set_addr_payload(CASC_ADDR_MASTER, 0xFF, 0); /* 新主卡格 = 0 */
    _casc_put_u32(p.claim, 400);
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_BCAST, APP_CASC_TYPE_SET_ADDR, 18, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_master_cell == 0, "主卡格应跟着更新到 0，得到 %u", (unsigned)s_master_cell);
    CHECK_MSG(s_self_addr == 1, "本卡仍在格 1，在新表里那就是 addr 1，得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_last_saved_mc == 0, "新的主卡格要落记录（掉电不忘）");
    CHECK_MSG(s_reinit_calls == 1, "格变了要重装门面（本卡渲染的矩形跟着变）");
}

/** @brief 识别帧 `yours` 的**两端取值**：0（主卡地址）不得点名 → 拒绝；count-1（合法上界）→ 采纳
 *
 *  本夹具是 1×2 网格 → 具体地址的合法集 = `1..count-1`，而 `0` 虽然网格里存在
 *  （它正是主卡地址），**作为指令一律拒绝**：主卡身份只由广播自算 + claim/按键产生，
 *  识别帧不得把接收方点名为主卡。从前这一支会**跳过 claim 仲裁**直接采纳 0 ——
 *  一张从卡被单播点名就自封主卡并落盘（两张主卡、一半屏停更），重启不忘。
 *  发侧本来也只发 `0xFF`（自算）或 `1..格数-1`（单播），所以拒掉 0 只是把"发侧约定"
 *  升格为**收侧强制**。
 *  上界这一条保留"上界不是排他边界"的意义：记错一位（写成 `>= count-1`）会被误拒，
 *  ② 立刻红。
 *
 *  （越界一侧与"门禁不能写成总线字段宽度"见下面的 out_of_grid 两案。） */
static void case_set_addr_yours_edges(void)
{
    TEST_BEGIN("识别帧 yours·两端：0（点名主卡）拒绝、count-1（合法上界）采纳");

    /* ① `yours == 0`（主卡地址）→ **拒绝**：主卡身份只由广播自算 + claim/按键产生，
       识别帧不得把接收方点名为主卡。 */
    fixture_reset();
    s_self_addr          = 1; /* 本机是从卡 */
    app_casc_set_addr_t p = set_addr_payload(CASC_ADDR_MASTER, CASC_ADDR_MASTER, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 21, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1 && s_master_cell == 1,
              "yours=0 不得把从卡点名为主卡（连格都不许动），得到 addr=%u mc=%u",
              (unsigned)s_self_addr, (unsigned)s_master_cell);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用（不落盘、不 apply）");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "点名主卡应以 NACK(ADDR) 明确回绝");

    /* ② `yours == 网格格数-1`（**合法上界**）→ 采纳。 */
    fixture_reset();
    s_self_addr = 1;
    p           = set_addr_payload(CASC_ADDR_MASTER, CASC_CELL_COUNT - 1U, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 22, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == CASC_CELL_COUNT - 1U, "合法上界（= 网格格数-1）应被采纳，得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == CASC_CELL_COUNT - 1U, "采纳了就要落记录");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_ACK, "采纳应回 ACK");
}

/** @brief 识别帧 `yours` **越界**（含"总线字段放得下、网格放不下"的值）→ 一律 NACK(ADDR) + 零副作用
 *
 *  **为什么需要它**：`yours` 是识别帧的**直接输入**，合法集 = 新切分表的地址集合
 *  `[0, nx*ny)`（本夹具 1×2 → `{0, 1}`）。曾经的实现没有任何范围校验，线上一个越界值
 *  会被原样采纳并落盘：本卡随即落到切分表外（门面停用、不落屏，RS485 单播也不再认它），
 *  而且**重启不会自愈** —— 读侧对 `> 0x1F` 的地址会拒掉、回落板级默认 0（= 主卡），
 *  本卡静默变成**第二张主卡**（两路 PING、各开各的轮，一半屏停更）。
 *
 *  注意 `0x1F` **不是"合法上界"**：它只是总线地址字段的宽度 —— 2 卡网格上
 *  `yours = 2` 同样会砖，所以判据只能用网格格数（`CASC_CELL_COUNT`）。
 *  每条拒绝都要求**零副作用**（身份、记录、门面一个都不许动）—— 光是"回了 NACK
 *  但顺手改了别的"在现场同样难查。⑤ 还钉住：越界值不许绕过**门禁之前**的 claim 仲裁。 */
static void case_set_addr_yours_out_of_grid(void)
{
    TEST_BEGIN("识别帧 yours·越界（count/0x1F/0x20/0xFE）：NACK(ADDR) + 零副作用，仲裁不被短路");

    /* ① `yours == 网格格数`（= 2，越界一位）→ 拒绝。它就是旧用例拿来做"单播改址"的
       那个值：采纳后本卡就落到切分表外。判据把 `>=` 写成 `>` 时，这一条立刻红。 */
    fixture_reset();
    s_self_addr          = 1;
    app_casc_set_addr_t p = set_addr_payload(CASC_ADDR_MASTER, CASC_CELL_COUNT, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 23, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1, "越界一位（yours=%u）不该改身份，得到 %u", (unsigned)CASC_CELL_COUNT,
              (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用（不落盘、不 apply）");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 yours 应以 NACK(ADDR) 明确回绝");

    /* ② `yours == 0x1F`：总线字段放得下、但 1×2 网格放不下 —— 它是 `dst` 字段的宽度
       （01..1F），**不是本板的合法集**。这条钉的正是"判据不能写成 `<= 0x1F`"。 */
    fixture_reset();
    s_self_addr = 1;
    p           = set_addr_payload(CASC_ADDR_MASTER, 0x1F, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 24, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1, "0x1F 只是总线字段宽度、不是合法集：不该采纳，得到 %u",
              (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 yours 应以 NACK(ADDR) 回绝");

    /* ③④ 0x20 / 0xFE：一个离 `0x1F` 差一位、一个离"自算"的 `0xFF` 差一位。
       判据若写成"字段值域"的形态（`> 0x1F`、`>= 0xFF`、位掩码），这两条会走岔。 */
    fixture_reset();
    s_self_addr = 1;
    p           = set_addr_payload(CASC_ADDR_MASTER, 0x20, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 25, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1 && s_save_calls == 0 && s_reinit_calls == 0, "0x20 应拒绝、零副作用");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 yours 应以 NACK(ADDR) 回绝");

    fixture_reset();
    s_self_addr = 1;
    p           = set_addr_payload(CASC_ADDR_MASTER, 0xFE, 1);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 26, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1 && s_save_calls == 0 && s_reinit_calls == 0, "0xFE 应拒绝、零副作用");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 yours 应以 NACK(ADDR) 回绝");

    /* ⑤ 仲裁不被非法值短路：本卡自称主卡、**按得更晚**（claim 更大）→ 照旧回绝。
       这条走的是**门禁之前**的 claim 分支（理由不同、结论一样）—— 少了它，
       `yours` 一旦越界就能绕过"最近按的那张卡作数"这条规则。 */
    fixture_reset();
    s_my_claim = 300; /* 本卡在 300ms 认领过（本机此刻是主卡 addr 0） */
    p          = set_addr_payload(CASC_ADDR_MASTER, 0xFE, 1);
    _casc_put_u32(p.claim, 200); /* 对方更早 */
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_MASTER, APP_CASC_TYPE_SET_ADDR, 29, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == CASC_ADDR_MASTER,
              "越界 yours 也要走 claim 仲裁：本卡按得更晚就不让位，得到 %u", (unsigned)s_self_addr);
    CHECK_MSG(s_save_calls == 0, "回绝时不该写记录");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK, "应明确回绝（静默会让对方一直等）");
}

/** @brief 识别帧 `master_cell` **越界**（`yours` 完全合法也拒）：NACK(ADDR)、身份与格都不动
 *
 *  今天"连地址都不用越界就能把卡写砖"的那条 —— 越界主卡格落盘后各卡渲染的格子对不上，
 *  重启读回来还是它。两个越界方向都测：`= 网格格数`（差一位）与 `0xFF`。
 *  只卡 `new_addr`、漏掉 `mc` 的实现会让这两条立刻红。
 *  "连格都不许动"（`s_master_cell` 不变）是**零副作用**的一半，必须与身份断言同案。 */
static void case_set_addr_mc_out_of_grid(void)
{
    TEST_BEGIN("识别帧 master_cell 越界（= count / 0xFF）：NACK(ADDR)、身份与格都不动");

    /* ① `mc == 网格格数`（差一位）→ 拒绝。 */
    fixture_reset();
    s_self_addr          = 1;
    app_casc_set_addr_t p = set_addr_payload(CASC_ADDR_MASTER, 1, CASC_CELL_COUNT);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 27, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1 && s_master_cell == 1,
              "mc=%u 越界不该动身份（连格都不许动），得到 addr=%u mc=%u", (unsigned)CASC_CELL_COUNT,
              (unsigned)s_self_addr, (unsigned)s_master_cell);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 mc 应以 NACK(ADDR) 回绝");

    /* ② `mc == 0xFF` → 拒绝。 */
    fixture_reset();
    s_self_addr = 1;
    p           = set_addr_payload(CASC_ADDR_MASTER, 1, 0xFF);
    peer_frame(CASC_ADDR_MASTER, 1, APP_CASC_TYPE_SET_ADDR, 28, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == 1 && s_master_cell == 1,
              "mc=0xFF 越界不该动身份，得到 addr=%u mc=%u", (unsigned)s_self_addr,
              (unsigned)s_master_cell);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "越界 mc 应以 NACK(ADDR) 回绝");
}

/** @brief 存量砖 × 广播自算：**救不回**（恒等映射）且不许把越界身份再写一遍
 *
 *  本卡身份已经越界（addr = 网格格数）时收到广播自算（`yours=0xFF`）：自算是
 *  **恒等映射**（越界地址不在表里，格↔址互推只在原地打转），算出来还是那个越界值
 *  → 门禁拒绝。即"用广播重编址把这块砖救回来"**不成立**；存量砖只能靠外部工具
 *  向越界地址发一条 `yours=合法` 的单播、或擦身份记录。 */
static void case_set_addr_brick_broadcast(void)
{
    TEST_BEGIN("越界身份 + 广播自算：拒绝、不落盘、地址与格都不动（广播救不回）");

    /* ① 砖（addr=网格格数）收到 `yours=0xFF` 的广播：拒绝要零副作用 —— 救不回，
       也不许把越界身份再写一遍。 */
    fixture_reset();
    s_self_addr   = CASC_CELL_COUNT; /* 砖：越界地址 */
    s_master_cell = 1;
    app_casc_set_addr_t p = set_addr_payload(CASC_ADDR_MASTER, CASC_ADDR_SELF_CALC, 0);
    _casc_put_u32(p.claim, 500);
    peer_frame(CASC_ADDR_MASTER, CASC_ADDR_BCAST, APP_CASC_TYPE_SET_ADDR, 30, &p, sizeof(p));
    _casc_drain();
    CHECK_MSG(s_self_addr == CASC_CELL_COUNT && s_master_cell == 1,
              "广播自算是恒等映射，救不回越界身份，得到 addr=%u mc=%u", (unsigned)s_self_addr,
              (unsigned)s_master_cell);
    CHECK_MSG(s_save_calls == 0 && s_reinit_calls == 0, "拒绝要零副作用（不落盘、不 apply）");
    CHECK_MSG(s_tx_last.type == APP_CASC_TYPE_NACK && s_tx_last.payload[0] == APP_CASC_NACK_ADDR,
              "应回 NACK(ADDR)，而不是静默");
}

/** @brief 按键认领：**按哪块哪块是主卡，且它那一格变成主卡格**（F3 的核心）
 *
 *  格不跟着走，现场就是那半幅内容对调 —— 而两块屏"各自都对"，看不出错。 */
static void case_claim_master_flow(void)
{
    TEST_BEGIN("按键认领：成为 addr 0 + **本卡那一格变成主卡格**，广播 + 逐卡单播");

    /* 本卡是**从卡**（addr 1，格 0），主卡格在下面那块（1）—— 现场那个排布。
       对端（假从卡）是**旧主卡**，此刻在 addr 0 上听着。 */
    fixture_reset();
    s_self_addr  = 1;
    s_slave.addr = 0;
    s_state[0]   = APP_SCREEN_CARD_STATE_ONLINE;
    s_state[1]   = APP_SCREEN_CARD_STATE_ONLINE;

    app_casc_claim_master();
    CHECK_MSG(s_claim_req_flag, "按键只投递请求（写 flash + 等 ACK 最长约 1s，必须由级联任务做）");
    CHECK_MSG(s_slave.set_addr_rx == 0, "投递请求时还不该发任何帧（动作在级联任务里）");

    const uint32_t t0 = osKernelGetTickCount();
    _claim_poll(); /* _casc_task 里那一格 */
    CHECK_MSG(!s_claim_req_flag, "取走请求后标志要清掉（否则每圈都认领一次）");

    /* **本卡那一格变成主卡格**，地址随之变成 0 —— 两块屏仍然各显示自己那半幅 */
    CHECK_MSG(s_master_cell == 0, "主卡格应变成本卡那一格（0），得到 %u", (unsigned)s_master_cell);
    CHECK_MSG(s_self_addr == CASC_ADDR_MASTER, "认领后本机就是主卡（addr 0）");
    CHECK_MSG(s_save_calls == 1 && s_last_saved_addr == 0 && s_last_saved_mc == 0,
              "认领要把「我是主卡 + 主卡格」一起落盘，得到 addr=%u mc=%u",
              (unsigned)s_last_saved_addr, (unsigned)s_last_saved_mc);
    CHECK_MSG(s_my_claim != 0xFFFFFFFFU, "认领时刻要记下来（两张都自称主卡时靠它裁决）");

    /* 通知：**广播一条**（谁在听谁自算）+ **逐卡单播**（按旧地址点名、给新地址） */
    CHECK_MSG(s_slave.bcast_set_addr_rx == 1, "应广播一条识别帧（对方未必枚举得到）");
    CHECK_MSG(s_slave.bcast_payload.mine == 0 &&
                  s_slave.bcast_payload.yours == 0xFF &&
                  s_slave.bcast_payload.master_cell == 0,
              "广播内容：mine=0 / yours=0xFF（自算）/ 主卡格=0");
    CHECK_MSG(s_slave.set_addr_rx == 1, "还应向表里那张卡单播一条，得到 %d 条",
              s_slave.set_addr_rx);
    CHECK_MSG(s_slave.set_addr_payload.yours == 1,
              "单播给旧主卡的新地址：主卡格翻到 0 之后，原来格 1 的它变成 addr 1（得到 %u）",
              (unsigned)s_slave.set_addr_payload.yours);
    CHECK_MSG(_casc_get_u32(s_slave.set_addr_payload.claim) == s_my_claim,
              "claim 必须是本卡认领那一刻的 tick");

    /* 认领会把协议侧按旧身份建立的陈旧状态全部作废，并**立刻**重新枚举 */
    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_MISSING, "认领后「谁在线」要全部作废");
    CHECK_MSG(s_reenum_at_tick >= t0 && s_reenum_at_tick <= osKernelGetTickCount(),
              "认领后应立刻重新枚举（否则新主卡要等下一个 10s 周期才被发现）");
}

/** @brief 身份变了：在途轮次必须自己收尾（不许按旧身份继续发、也不许提交**已被重装清空**的画布），
 *         而且这次失败不算该卡的（算了的话几轮认领下来会把一张好卡剔除） */
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

/** @brief `_identity_poke`：身份一变，按**旧身份**建立的陈旧状态（在线表、失败计数、
 *         开轮请求、应答槽）必须全部作废 —— 留着它们会让新身份按旧表工作 */
static void case_identity_poke(void)
{
    TEST_BEGIN("身份变了 → 作废陈旧状态并立刻重新枚举");

    fixture_reset();
    s_state[0]         = APP_SCREEN_CARD_STATE_ONLINE;
    s_state[1]         = APP_SCREEN_CARD_STATE_ONLINE;
    s_fail_run[0]      = 3;
    s_fail_run[1]      = 2;
    s_ack_state.valid  = true;
    s_force_round_flag = true;

    const uint32_t t0 = osKernelGetTickCount();
    _identity_poke();

    CHECK_MSG(app_screen_card_state(0) == APP_SCREEN_CARD_STATE_MISSING &&
                  app_screen_card_state(1) == APP_SCREEN_CARD_STATE_MISSING,
              "各卡状态必须回 MISSING：新身份要先重新枚举才知道谁在");
    CHECK_MSG(s_fail_run[0] == 0 && s_fail_run[1] == 0, "旧的失败计数同样作废");
    CHECK_MSG(!s_force_round_flag, "开轮请求不该跨身份继续（画布已重装）");
    CHECK_MSG(!s_ack_state.valid, "等应答槽要作废（那是旧身份那一轮的回应）");
    CHECK_MSG(s_reenum_at_tick >= t0 && s_reenum_at_tick <= osKernelGetTickCount(),
              "应把下一次枚举提前到**现在**，而不是等 10s 周期");
}

/** @brief 开轮三闸：画布没写过不开 / **渲染途中不开** / 静默期没到不开（force 也要过前两闸）
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
    s_canvas_touched_flag = false;
    s_settled             = true;
    s_render_busy         = false;
    CHECK_MSG(!_round_ready(), "画布没被写过时不许开轮（上电恢复的内容会被刷黑）");

    /* ② 画布写过 + 静默期到 → 开 */
    s_canvas_touched_flag = true;
    s_settled             = true;
    CHECK_MSG(_round_ready(), "画布写过且静默期已过应开轮");

    /* ③ **渲染途中**：静默期到了也不许（这一眼画布上只有半张，甚至刚清空） */
    s_canvas_touched_flag = true;
    s_settled             = true;
    s_render_busy         = true;
    CHECK_MSG(!_round_ready(), "渲染途中不许开轮 —— 推下去的就是那一帧「闪一下」的中间态");
    s_render_busy = false;

    /* ④ 静默期没到：内容还在变，攒着 */
    s_canvas_touched_flag = true;
    s_settled             = false;
    CHECK_MSG(!_round_ready(), "静默期没到不许开轮");

    /* ⑤ `force_round`（有卡刚上线）**也要过前两闸**，而且请求不许被白白吃掉 */
    s_settled             = false;
    s_canvas_touched_flag = false;
    s_force_round_flag    = true;
    CHECK_MSG(!_round_ready() && s_force_round_flag,
              "新上线的卡也要等画布被写过 —— 且这一眼的请求必须留着");
    s_canvas_touched_flag = true;
    s_render_busy         = true;
    CHECK_MSG(!_round_ready() && s_force_round_flag, "渲染途中 force_round 同样不许开");
    s_render_busy = false;
    CHECK_MSG(_round_ready() && !s_force_round_flag, "闸都过了才开，并把 force 请求清掉");

    /* ⑥ 请求被清掉之后，下一下不再开（依赖画布内容，而不是依赖这个标志） */
    s_settled = false;
    CHECK_MSG(!_round_ready(), "force 用掉之后要靠静默期那条路");
}

/* ================================================================
 *  看门狗
 *
 *  本套件是这批里唯一走**阻塞等待**的（`_round_run` → `_wait_ack` →
 *  `osMessageQueueGet(..., timeout_ms)`，上界 `CASC_ACK_TIMEOUT_MS`）。
 *  生产侧一旦把超时改成 `osWaitForever`、或等待循环丢掉 deadline 判断，
 *  症状是**挂死**而不是失败 —— 在 CI 里挂死比失败更糟（没有诊断、还占住机器）。
 *  用 SIGALRM 把它转成明确的失败退出（范式：test_dispatch.c 的看门狗）。
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（等待应答/开轮疑似永不返回）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36m级联图传 · 主卡侧（本板单卡 %ux%u，一帧 %u 字节）\033[0m\n", (unsigned)CARD_W,
           (unsigned)CARD_H, (unsigned)(CASC_OVERHEAD + sizeof(app_casc_image_t) + CARD_BM));

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
    case_id_resolve_bad_record();
    case_set_addr_rejected();
    case_set_addr_applied();
    case_set_addr_yours_edges();
    case_set_addr_yours_out_of_grid();
    case_set_addr_mc_out_of_grid();
    case_set_addr_brick_broadcast();
    case_claim_master_flow();
    case_round_aborts_on_identity_change();
    case_round_ready_gates();
    case_identity_poke();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
