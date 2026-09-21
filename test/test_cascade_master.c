/**
 * @file    test_cascade_master.c
 * @brief   级联图传 · **主卡侧**：一轮的开轮、分片下发、结算与定向重传
 *
 * **为什么需要这个测试**：主卡这一侧错了，现场看到的是"某块屏上不对"或"整轮变慢"，
 * 而没有任何日志会说是哪一步错的。三条只有在这儿才测得到：
 *
 *   1. **地址与矩形错配** —— 协议按地址寻址、切分表按下标索引，两者顺序**可以相反**
 *      （主卡在下时相反）。拿地址当下标就会把两块屏的内容**对调**，而 CRC、长度、
 *      几何校验**全部通过**。用例直接断言"发出去的 BEGIN 带的是那张卡自己的矩形"。
 *   2. **定向重传退化成熟重传** —— 丢一片就重发整轮，现场表现为"偶尔整屏闪一下"
 *      且一轮从 130ms 涨到 400ms。用例数每一片**被发了几次**。
 *   3. **从卡不回 ACK 时主卡冻结** —— 主卡自己的屏跟着一起不更新，是最坏的失效形态
 *      （整块屏全停）。用例断言"没有任何卡应答时主卡仍然提交本地"。
 *
 * 总线是假的：`ccb_send` 旁边坐着一个**假从卡**，它按协议收分片、组 ACK，再经
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

/* 分片数：3833024 的卡位图正好是一片（512 == CASC_FRAG_BYTES），
   5006048 是 1400 → 3 片。多片的用例在单片板上自动退化成单片。 */
#define FRAG_N ((CARD_BM + CASC_FRAG_BYTES - 1U) / CASC_FRAG_BYTES)

/* ---- 本卡：主卡（addr 0） ---- */
static bool s_is_master = true;
bool        app_screen_is_master(void) { return s_is_master; }
uint8_t     app_screen_self_addr(void) { return 0; }

/* ---- app_screen 替身 ---- */

/* **按现场的真实拓扑排**：上下拼，**下面那块是主卡**。
   于是 下标 0 是上面的从卡（addr 1）、下标 1 是下面的主卡（addr 0）——
   **addr 与下标正好相反**，这正是"拿地址当下标"最容易出错、也最该被测到的形态。
   （若按 addr==下标 排，那种错法在这套用例里完全测不出来。） */
#define SELF_IDX (1U) /* 本卡（主卡 addr 0）落在下标 1 */

static screen_card_t s_cards[2] = {
    {.addr = 1, .color = COLOR_RED, .x = 0, .y = 0, .w = CARD_W, .h = CARD_H},      /* 上：从卡 */
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
uint8_t                app_screen_self_index(void) { return SELF_IDX; }

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
bool app_screen_take_pending_settled(void) { return false; }

/* ---- 显示与总线 ---- */
#define DEV_W (48U)
#define DEV_H (16U)
static dev_display_t s_dev;
static uint8_t       s_pixel_map[DEV_W * DEV_H];
dev_display_t       *dev_display_get(void) { return &s_dev; }

/* ---- 被测：生产源码本体 ---- */
#include "../Application/Src/CASCADE/app_cascade.c"

/* ================================================================
 *  假从卡 —— 坐在总线另一头，按协议收分片、组 ACK
 * ================================================================ */

static struct {
    bool     begun;
    uint16_t seq;
    uint8_t  frag_n;
    uint16_t bmp_len;
    uint8_t  stage[CARD_BM];
    uint8_t  have;

    /* 注入 */
    uint8_t drop_mask;      /**< **只丢首发**的分片（模拟总线瞬时丢帧，重传能过） */
    uint8_t perm_drop_mask; /**< 一直丢的分片（模拟持续干扰，重传也补不上） */
    bool    silent;      /**< 永不回 ACK（模拟卡掉线） */
    bool    nack_on_begin;

    /* 观察 */
    int data_rx[CASC_FRAG_MAX]; /**< 每片**收到过几次** —— 定向重传靠它验证 */
    int begin_rx;
    int commit_rx;
    uint8_t  last_begin_x[2]; /**< 最近一次 BEGIN 里的矩形 x（大端） */
    uint8_t  last_begin_y[2]; /**< 最近一次 BEGIN 里的矩形 y（大端） */
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

/** @brief 假从卡回一帧给主卡（dst=0, src=1） */
static void slave_reply(uint8_t type, uint16_t seq, const void *payload, uint16_t plen)
{
    uint8_t        f[CASC_FRAME_MAX];
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(f, 0, len);
    f[0]            = CASC_SOF0;
    f[1]            = CASC_SOF1;
    f[2]            = (uint8_t)((CASC_PROTO_VER << 6) | type);
    f[3]            = 0; /* dst = 主卡 */
    f[4]            = 1; /* src = 本从卡 */
    casc_put_u16(f + 5, seq);
    casc_put_u16(f + 9, len);
    if (plen) memcpy(f + 11, payload, plen);
    casc_put_u32(f + len - 4U, pl_crc32_calc(pl_crc_get_handle(), f + 2, len - 6U));

    rb_write(&s_rb, f, len, nullptr);
    dispatch_pending();
}

static void slave_send_ack(uint16_t seq, uint8_t sta, uint8_t miss)
{
    const casc_ack_t a = {.sta = sta, .miss_mask = miss};
    slave_reply(CASC_T_ACK, seq, &a, sizeof(a));
}

static uint8_t s_tx_count; /**< 主卡一共发了多少帧 */

/** @brief 假从卡：收主卡发来的帧 */
static void slave_on_master_frame(const uint8_t *d, uint16_t l)
{
    const uint8_t  type = CASC_TYPE_OF(d[2]);
    const uint16_t seq  = casc_get_u16(d + 5);

    /* 总线是共享的，但**不是发给本卡的帧一律不理** —— 与真从卡一样。
       少了这条，"主卡把帧发给了自己（dst=0）"这种错会被当成正常收到。 */
    if (d[3] != 1) return;

    switch (type) {
    case CASC_T_SYNC_BEGIN: {
        const casc_sync_begin_t *p = (const casc_sync_begin_t *)(d + 11);
        s_slave.begun   = true;
        s_slave.seq     = seq;
        s_slave.frag_n  = p->frag_n;
        s_slave.bmp_len = casc_get_u16(p->bmp_len);
        s_slave.have    = 0;
        s_slave.begin_rx++;
        memcpy(s_slave.last_begin_x, p->x, 2);
        memcpy(s_slave.last_begin_y, p->y, 2);
        if (s_slave.nack_on_begin) {
            const casc_nack_t n = {.err = CASC_NACK_GEOM};
            slave_reply(CASC_T_NACK, seq, &n, sizeof(n));
        }
        break;
    }
    case CASC_T_SYNC_DATA: {
        const uint8_t idx = d[7];
        if (!s_slave.begun || idx >= CASC_FRAG_MAX) break;
        s_slave.data_rx[idx]++;

        /* **只丢首发**（瞬时错误）：重传要能过，否则测的是"永久丢"而不是重传 */
        if ((s_slave.drop_mask & (uint8_t)(1U << idx)) && s_slave.data_rx[idx] == 1) break;
        /* 永久丢：连着丢到底，用来验证"重试次数封顶后放弃" */
        if (s_slave.perm_drop_mask & (uint8_t)(1U << idx)) break;

        const uint16_t off = (uint16_t)(idx * CASC_FRAG_BYTES);
        const uint16_t n   = (uint16_t)(l - CASC_OVERHEAD);
        if ((uint32_t)off + n <= sizeof(s_slave.stage)) {
            memcpy(&s_slave.stage[off], d + 11, n);
            s_slave.have |= (uint8_t)(1U << idx);
        }
        break;
    }
    case CASC_T_SYNC_COMMIT: {
        s_slave.commit_rx++;
        if (s_slave.silent) break; /* 卡掉线：一句话都不回 */
        const uint8_t full = (uint8_t)((1U << s_slave.frag_n) - 1U);
        const uint8_t miss = (uint8_t)(full & ~s_slave.have);
        slave_send_ack(seq, miss ? CASC_ACK_MISS : CASC_ACK_OK, miss);
        break;
    }
    default:
        break;
    }
}

void ccb_send(ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    s_tx_count++;
    if (l >= CASC_OVERHEAD) slave_on_master_frame(d, l);
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

/** 每一片各发了几次（按本板的分片数取前 FRAG_N 项） */
static void check_data_counts(const int *want, const char *what)
{
    for (uint8_t k = 0; k < FRAG_N; k++) {
        if (s_slave.data_rx[k] != want[k]) {
            CHECK_MSG(0, "%s：第 %u 片发了 %d 次，期望 %d 次", what, (unsigned)k,
                      s_slave.data_rx[k], want[k]);
            return;
        }
    }
    CHECK_MSG(1, "%s", what);
}

/* ================================================================ */

/** 正常一轮：分片逐片到齐，从卡收齐后主卡才提交本地 */
static void case_round_ok(void)
{
    TEST_BEGIN("正常一轮：分片到齐、从卡收齐、主卡提交本地");

    fixture_reset();
    _round_run();

    CHECK_MSG(s_slave.begin_rx == 1, "从卡应收到 1 帧 BEGIN，得到 %d", s_slave.begin_rx);
    CHECK_MSG(s_slave.commit_rx >= 1, "从卡应收到 COMMIT，得到 %d", s_slave.commit_rx);

    int want[CASC_FRAG_MAX] = {0};
    for (uint8_t k = 0; k < FRAG_N; k++) want[k] = 1;
    check_data_counts(want, "每片只发一次");

    /* 从卡拼出来的位图必须与"主卡抽给**它**的那块"逐字节相等 —— 抽的是下标 0
       （从卡自己），不是下标 SELF_IDX（主卡自己） */
    uint8_t expect[CARD_BM];
    card_pattern(0, expect, CARD_BM);
    CHECK_MSG(s_slave.bmp_len == CARD_BM, "从卡理解的位图长度应为 %u，得到 %u",
              (unsigned)CARD_BM, (unsigned)s_slave.bmp_len);
    CHECK_MSG(memcmp(s_slave.stage, expect, CARD_BM) == 0, "从卡拼出来的内容与主卡抽出的不一致");

    CHECK_MSG(s_commit_self_calls == 1, "主卡应提交本地一次，得到 %d", s_commit_self_calls);
}

/** 发出去的 BEGIN 必须带**那张卡自己**的矩形 —— 地址与下标不许错配 */
static void case_rect_matches_addr(void)
{
    TEST_BEGIN("BEGIN 带的是该卡自己的矩形（地址与下标不许错配）");

    fixture_reset();
    _round_run();

    /* 发给 addr 1 的必须是**它自己**那块（下标 0 = 上半屏 y=0），
       而不是主卡那块（下标 SELF_IDX = 下半屏 y=CARD_H）。 */
    const uint16_t y = casc_get_u16(s_slave.last_begin_y);
    CHECK_MSG(y == 0, "发给 addr 1 的矩形 y 应为 0（上半屏），得到 %u（=%u 说明把主卡那块发过去了）",
              (unsigned)y, (unsigned)CARD_H);

    const uint8_t  bm_idx = casc_get_u16(s_slave.last_begin_y) / CARD_H;
    uint8_t        expect_self[CARD_BM];
    card_pattern(SELF_IDX, expect_self, CARD_BM);
    const uint8_t pattern_idx = (bm_idx == SELF_IDX) ? SELF_IDX : 0;
    uint8_t       expect_recv[CARD_BM];
    card_pattern(pattern_idx, expect_recv, CARD_BM);

    /* 自检：**本卡与从卡的图案必须不同、下标与地址必须不同** ——
       否则"发错卡"和"拿地址当下标"这两条根本测不出来。 */
    CHECK_MSG(memcmp(expect_self, expect_recv, CARD_BM) != 0, "本夹具两张卡的图案必须不同");
    CHECK_MSG(s_cards[0].addr != 0 && s_cards[SELF_IDX].addr == 0,
              "本夹具必须让 addr 与下标**不同序**，否则这条用例没有分辨力");
    CHECK_MSG(s_cards[0].y != s_cards[SELF_IDX].y,
              "本夹具两张卡的矩形必须**不同位**，否则「发错卡」测不出来");

    /* 本卡（addr 0）那块**不该**被下发 —— 它由本地提交处理 */
    CHECK_MSG(s_extract_calls == 1, "只应抽取从卡那一块（本卡那块走 commit_self），抽了 %d 次",
              s_extract_calls);
}

/** 丢一片：只补那一片，不是重发整轮 */
static void case_targeted_retransmit(void)
{
    TEST_BEGIN("丢一片 → 定向重传（只补缺的那片）");

    fixture_reset();
    s_slave.drop_mask = 0x01; /* 只丢第 0 片 */
    _round_run();

    /* 第 0 片发两次（首发 + 补发），其余各一次 ——
       3833024 的卡位图正好一片（FRAG_N==1），"只补缺的那片"在这儿退化成同一件事。 */
    int want[CASC_FRAG_MAX] = {0};
    for (uint8_t k = 0; k < FRAG_N; k++) want[k] = 1;
    want[0] = 2;
    check_data_counts(want, FRAG_N > 1 ? "只补了缺的那一片，其余没重发" : "单片板：补发那一片");

    uint8_t expect[CARD_BM];
    card_pattern(0, expect, CARD_BM); /* 从卡 = 下标 0 */
    CHECK_MSG(memcmp(s_slave.stage, expect, CARD_BM) == 0, "补片后从卡的内容仍应与主卡抽出的一致");
    CHECK_MSG(s_commit_self_calls == 1, "补片成功后主卡应提交本地");
}

#if FRAG_N > 1
/** 丢中间一片：确认补的是**中间那片**，不是从头重来 */
static void case_targeted_retransmit_middle(void)
{
    TEST_BEGIN("丢中间那片 → 补的也是中间那片");

    fixture_reset();
    s_slave.drop_mask = 0x02;
    _round_run();

    int want[CASC_FRAG_MAX] = {0};
    for (uint8_t k = 0; k < FRAG_N; k++) want[k] = 1;
    want[1] = 2;
    check_data_counts(want, "只有第 1 片被补发");

    uint8_t expect[CARD_BM];
    card_pattern(0, expect, CARD_BM); /* 从卡 = 下标 0 */
    CHECK_MSG(memcmp(s_slave.stage, expect, CARD_BM) == 0, "补片后从卡的内容仍应与主卡抽出的一致");
}
#endif

/** 从卡始终不回 ACK：主卡**不许冻结**，本地照常更新 */
static void case_silent_slave_master_still_commits(void)
{
    TEST_BEGIN("从卡不回 ACK → 主卡仍提交本地（整块屏不许跟着停）");

    fixture_reset();
    s_slave.silent = true;

    const uint32_t t0 = osKernelGetTickCount();
    _round_run();
    const uint32_t dt = osKernelGetTickCount() - t0;

    CHECK_MSG(s_commit_self_calls == 1, "从卡一句话不回时，主卡**仍然**要提交本地，得到 %d 次",
              s_commit_self_calls);

    /* 重试有限度：一次首发 + CASC_RETRY_MAX 次重传，不能无限试下去 */
    const int want_each = 1 + (int)CASC_RETRY_MAX;
    int       want[CASC_FRAG_MAX] = {0};
    for (uint8_t k = 0; k < FRAG_N; k++) want[k] = want_each;
    check_data_counts(want, "重传次数被 CASC_RETRY_MAX 封顶");

    CHECK_MSG(s_slave.commit_rx == want_each, "COMMIT 应发 %d 次，得到 %d", want_each,
              s_slave.commit_rx);
    CHECK_MSG(dt < 1000U, "一轮耗时应在一秒内（卡掉线时也不该拖长），实测 %ums",
              (unsigned)dt);
}

/** 一片**永久**丢：重试封顶后放弃该卡，主卡自己照常更新 */
static void case_permanent_loss_gives_up(void)
{
    TEST_BEGIN("一片持续丢 → 重试封顶后放弃，主卡不跟着卡死");

    fixture_reset();
    s_slave.perm_drop_mask = 0x01;

    const uint32_t t0 = osKernelGetTickCount();
    _round_run();
    const uint32_t dt = osKernelGetTickCount() - t0;

    const int want_each = 1 + (int)CASC_RETRY_MAX;
    int       want[CASC_FRAG_MAX] = {0};
    for (uint8_t k = 0; k < FRAG_N; k++) want[k] = 1;
    want[0] = want_each;
    check_data_counts(want, "只反复补那一片，其余各一次");

    CHECK_MSG(s_commit_self_calls == 1, "一张卡补不上不该拖住主卡自己的更新");
    CHECK_MSG(dt < 1000U, "放弃要及时（重试封顶），实测 %ums", (unsigned)dt);
}

/** 从卡回 NACK（几何不符）：立刻放弃这张卡，不再 COMMIT、不再重传 */
static void case_nack_gives_up(void)
{
    TEST_BEGIN("从卡回 NACK → 立刻放弃该卡，不做无用的重传");

    fixture_reset();
    s_slave.nack_on_begin = true;
    _round_run();

    CHECK_MSG(s_slave.commit_rx == 0, "被 NACK 的卡不该再收到 COMMIT，得到 %d 次",
              s_slave.commit_rx);
    CHECK_MSG(s_commit_self_calls == 1, "一张卡参与不了，主卡自己仍要更新画面");
}

/** 整轮不许重复抽取：每张从卡一次 */
static void case_one_extract_per_card(void)
{
    TEST_BEGIN("每张从卡只抽一次（重传用的是已经抽好的那块）");

    fixture_reset();
    s_slave.drop_mask = 0x01;
    _round_run();

    CHECK_MSG(s_extract_calls == 1, "一次抽取 + 一次重传也只该抽 1 次，抽了 %d 次",
              s_extract_calls);
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m级联图传 · 主卡侧（本板单卡 %ux%u，%u 片/轮）\033[0m\n", (unsigned)CARD_W,
           (unsigned)CARD_H, (unsigned)FRAG_N);

    case_round_ok();
    case_rect_matches_addr();
    case_targeted_retransmit();
#if FRAG_N > 1
    case_targeted_retransmit_middle();
#endif
    case_silent_slave_master_still_commits();
    case_permanent_loss_gives_up();
    case_nack_gives_up();
    case_one_extract_per_card();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
