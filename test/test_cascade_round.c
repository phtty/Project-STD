/**
 * @file    test_cascade_round.c
 * @brief   级联图传 · **从卡侧**：收分片 → 暂存 → 收齐才落屏 → 回 ACK/NACK
 *
 * **为什么需要这个测试**：从卡这一侧错了，现场看到的是"某块屏上少一条"或"显示了
 * 上一轮的内容"，而两块屏分别在两台设备上，没有任何一处日志会指出是哪一步错的。
 * 尤其三条**静默错**，它们都不会报错、不会超时，只是画面不对：
 *
 *   1. **陈旧分片**——上一轮的迟到分片帧头完好、CRC 也对，探针照常放行。不靠 seq
 *      拦就会写进本轮暂存，画面变成"一半旧内容一半新内容"。
 *   2. **短分片**——长度短一截的分片若不拒，会静默留下旧字节，而 have_mask 记成
 *      "这片到了"，主卡便再也不补发，错内容永久留在屏上。
 *   3. **缺片落屏**——边收边落会出半幅画面。
 *
 * 用例走的是**真探针 + 真分派表**（`casc_probe_frame` → `g_casc_cmd[]`），
 * 只有总线与 app_screen 是替身；帧由用例独立构造，不复用被测的 `_build`。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_cascade.h"
#include "app_screen.h"
#include "dev_display.h"
#include "ring_buffer.h"

/* ---- 替身：总线与整屏门面 ---- */

static uint8_t  s_tx_frame[CASC_FRAME_MAX];
static uint16_t s_tx_len;
static int      s_tx_count;

int32_t ccb_send(ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    if (l <= sizeof(s_tx_frame)) {
        memcpy(s_tx_frame, d, l);
        s_tx_len = l;
    }
    s_tx_count++;
    return (int32_t)l;
}
ccb_t *app_rs485_ccb(void) { return nullptr; }
void   app_proto_bind(pcb_t *p, ccb_t *c) { (void)p; (void)c; }
void   pl_task_new_stub(void) {}

/* 本卡身份：默认是**从卡**（addr=1）。case_master_ignores 会临时翻过来。 */
static bool s_is_master;
bool        app_screen_is_master(void) { return s_is_master; }
uint8_t     app_screen_self_addr(void) { return 1; }

/* 落屏替身：把内容留下来供断言 —— 从卡的落屏路径就是"收齐了调一次 commit_bitmap"，
   留证比拉进真的 app_screen.c（要带上切分表与画布一整串）更直接。 */
static uint8_t  s_commit_bm[256];
static uint16_t s_commit_len;
static uint8_t  s_commit_color;
static int      s_commit_count;

void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color)
{
    if (len <= sizeof(s_commit_bm)) {
        memcpy(s_commit_bm, bm, len);
        s_commit_len = len;
    }
    s_commit_color = color;
    s_commit_count++;
}

static uint8_t s_bright_last = 0xFF;
static int     s_bright_calls;
void           app_screen_set_brightness(uint8_t l)
{
    s_bright_last = l;
    s_bright_calls++;
}
uint8_t app_screen_get_brightness(void) { return s_bright_last; }
bool    app_screen_brightness_take_pending(uint8_t *l)
{
    (void)l;
    return false;
}

/* 本卡实屏：几何必须与 BEGIN 里的矩形一致，否则从卡回 NACK —— 这正是被测行为之一 */
#define DEV_W (48U)
#define DEV_H (16U)
static dev_display_t s_dev;
static uint8_t       s_pixel_map[DEV_W * DEV_H];

dev_display_t *dev_display_get(void) { return &s_dev; }

/* ---- 被测：生产源码本体（探针与分派表都是 static） ---- */
#include "../Application/Src/CASCADE/app_cascade.c"

/* ================================================================
 *  夹具
 * ================================================================ */

RB_DEFINE(s_rb, 4096);

static uint8_t s_msg_buf[sizeof(frame_msg_t) + FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));
static frame_msg_t *s_msg = (frame_msg_t *)s_msg_buf;

#define BMP_LEN    (96U) /* ceil(48/8) × 16 */
#define FRAG_BYTES (40U) /* 3 片：40 + 40 + 16（末片更短，专测那条路径） */
#define FRAG_N     (3U)
#define SEQ        (7U)

static uint8_t s_bmp[BMP_LEN];
static uint8_t s_frag[FRAG_N][FRAG_BYTES];

/** 位图图案：每字节都不同，缺片 / 错位 / 旧字节残留都会露出来 */
static void bmp_pattern(void)
{
    for (uint16_t i = 0; i < BMP_LEN; i++) s_bmp[i] = (uint8_t)(i * 7U + 3U);
    /* 按分片切开，便于逐片发送 */
    for (uint8_t k = 0; k < FRAG_N; k++) {
        const uint16_t off = (uint16_t)(k * FRAG_BYTES);
        const uint16_t n   = (uint16_t)((BMP_LEN - off > FRAG_BYTES) ? FRAG_BYTES : (BMP_LEN - off));
        memcpy(s_frag[k], &s_bmp[off], n);
    }
}

static void fixture_reset(void)
{
    s_rb.read_index  = 0;
    s_rb.write_index = 0;
    s_casc_pcb.rb    = &s_rb;

    s_rx.active    = false;
    s_rx.have_mask = 0;

    s_tx_len   = 0;
    s_tx_count = 0;

    s_commit_count = 0;
    s_commit_len   = 0;
    s_commit_color = 0;

    s_bright_calls = 0;
    s_is_master    = false;

    s_dev.screen_rows = DEV_W;
    s_dev.screen_cols = DEV_H;
    s_dev.pixel_map   = s_pixel_map;
    memset(s_pixel_map, 0, sizeof(s_pixel_map));

    bmp_pattern();
}

/** @brief 把一帧喂进**真探针**，READY 后交给**真分派表** —— 与框架走同一条路径
 *
 *  探针窥视的目标与入参缓冲是同一块内存（生产里也是这么做的，见 app_dispatch.c
 *  的说明），所以 READY 之后帧内容已经在 `s_msg->data` 里了。 */
static bool feed(const uint8_t *f, uint16_t len)
{
    rb_write(&s_rb, f, len, nullptr);

    uint32_t        tl  = 0;
    uint8_t         aux = 0;
    pcb_probe_sta_t st =
        casc_probe_frame(&s_casc_pcb, nullptr, nullptr, s_msg->data, FRAME_DATA_MAX_LEN, &tl, &aux);

    if (st != PCB_PROBE_READY) {
        rb_skip(&s_rb, 1, nullptr); /* 伪帧：与框架一样跳 1 字节重试 */
        return false;
    }
    rb_skip(&s_rb, tl, nullptr);

    s_msg->data_len = (uint16_t)tl;
    s_msg->aux      = aux;
    s_msg->ccb      = nullptr;

    if (aux <= CASC_TYPE_MASK && g_casc_cmd[aux]) g_casc_cmd[aux](s_msg);
    return true;
}

/* ---- 帧构造：**独立**实现，不复用被测的 _build ---- */

static uint16_t build(uint8_t *out, uint8_t type, uint16_t seq, uint8_t idx, uint8_t frag_n,
                      const void *payload, uint16_t plen)
{
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(out, 0, len);
    out[0] = CASC_SOF0;
    out[1] = CASC_SOF1;
    out[2] = (uint8_t)((CASC_PROTO_VER << 6) | type);
    out[3] = 1;                    /* dst = 本卡（addr 1） */
    out[4] = CASC_ADDR_MASTER;     /* src = 主卡 */
    casc_put_u16(out + 5, seq);
    out[7] = idx;
    out[8] = frag_n;
    casc_put_u16(out + 9, len);
    if (plen) memcpy(out + 11, payload, plen);
    casc_put_u32(out + len - 4U, pl_crc32_calc(pl_crc_get_handle(), out + 2, len - 6U));
    return len;
}

static casc_sync_begin_t begin_payload(uint16_t w, uint16_t h, uint16_t bmp_len,
                                       uint16_t frag_bytes, uint8_t frag_n, uint8_t bright,
                                       uint8_t color)
{
    casc_sync_begin_t p;
    memset(&p, 0, sizeof(p));
    casc_put_u16(p.x, 0);
    casc_put_u16(p.y, 0);
    casc_put_u16(p.w, w);
    casc_put_u16(p.h, h);
    casc_put_u16(p.bmp_len, bmp_len);
    casc_put_u16(p.frag_bytes, frag_bytes);
    p.frag_n = frag_n;
    p.bright = bright;
    p.color  = color;
    return p;
}

/** @brief 发一帧 BEGIN（默认参数 = 本夹具的几何与分片） */
static void send_begin(uint16_t seq, uint8_t bright)
{
    uint8_t                 f[64];
    const casc_sync_begin_t p =
        begin_payload(DEV_W, DEV_H, BMP_LEN, FRAG_BYTES, FRAG_N, bright, COLOR_GREEN);
    const uint16_t n = build(f, CASC_T_SYNC_BEGIN, seq, 0, 0, &p, sizeof(p));
    feed(f, n);
}

/** @brief 发一帧 DATA；plen 用来构造"长度不对的分片" */
static void send_data(uint16_t seq, uint8_t idx, uint16_t plen)
{
    uint8_t f[CASC_FRAME_MAX];
    const uint16_t want =
        (uint16_t)((BMP_LEN - idx * FRAG_BYTES > FRAG_BYTES) ? FRAG_BYTES
                                                             : (BMP_LEN - idx * FRAG_BYTES));
    const uint16_t n = (plen == 0) ? want : plen;
    const uint16_t l = build(f, CASC_T_SYNC_DATA, seq, idx, FRAG_N, s_frag[idx], n);
    feed(f, l);
}

static void send_commit(uint16_t seq)
{
    uint8_t        f[32];
    const uint16_t n = build(f, CASC_T_SYNC_COMMIT, seq, 0, 0, nullptr, 0);
    feed(f, n);
}

static void send_abort(uint16_t seq)
{
    uint8_t        f[32];
    const uint16_t n = build(f, CASC_T_SYNC_ABORT, seq, 0, 0, nullptr, 0);
    feed(f, n);
}

/** @brief 三次 DATA 全发 */
static void send_all_data(uint16_t seq)
{
    for (uint8_t k = 0; k < FRAG_N; k++) send_data(seq, k, 0);
}

/* ---- 收到的应答 ---- */

static bool             last_is(uint8_t type)
{
    return s_tx_len >= CASC_OVERHEAD && CASC_TYPE_OF(s_tx_frame[2]) == type;
}
static const casc_ack_t *last_ack(void) { return (const casc_ack_t *)(s_tx_frame + 11); }

/* ---- 断言 ---- */
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

/* ================================================================ */

/** 完整一轮：收齐三片 → COMMIT → 落屏内容与发出去的一致 */
static void case_round_ok(void)
{
    TEST_BEGIN("完整一轮：三片收齐才落屏，内容逐字节相等");

    fixture_reset();
    send_begin(SEQ, 4);
    CHECK_MSG(s_commit_count == 0, "BEGIN 之后不该落屏（那会落出全黑一帧）");

    send_all_data(SEQ);
    CHECK_MSG(s_commit_count == 0, "DATA 阶段不该落屏（边收边落会出半幅画面）");
    CHECK_MSG(s_rx.have_mask == 0x07, "三片应全部记到，have_mask=%02X", s_rx.have_mask);

    send_commit(SEQ);
    CHECK_MSG(s_commit_count == 1, "COMMIT 后应恰好落屏一次，得到 %d 次", s_commit_count);
    CHECK_MSG(s_commit_len == BMP_LEN, "落屏长度应为 %u，得到 %u", (unsigned)BMP_LEN,
              (unsigned)s_commit_len);
    CHECK_MSG(memcmp(s_commit_bm, s_bmp, BMP_LEN) == 0, "落屏内容与发出去的不一致");
    CHECK_MSG(s_commit_color == COLOR_GREEN, "落屏颜色应取 BEGIN 里的本卡颜色，得到 %u",
              (unsigned)s_commit_color);

    CHECK_MSG(last_is(CASC_T_ACK), "应回一帧 ACK");
    CHECK_MSG(last_ack()->sta == CASC_ACK_OK, "应回 OK，得到 sta=%u",
              (unsigned)last_ack()->sta);
    CHECK_MSG(last_ack()->miss_mask == 0, "OK 时 miss_mask 应为 0");
}

/** 缺片 → 定向重传：ACK 报出缺哪片，补上后才落屏 */
static void case_missing_fragment(void)
{
    TEST_BEGIN("缺片：ACK 报出缺哪一片，补上后才落屏");

    fixture_reset();
    send_begin(SEQ, 4);
    send_data(SEQ, 0, 0);
    send_data(SEQ, 2, 0); /* 故意不发第 1 片 */
    send_commit(SEQ);

    CHECK_MSG(last_is(CASC_T_ACK) && last_ack()->sta == CASC_ACK_MISS,
              "缺片时应回 MISS（主卡据此只补那一片）");
    CHECK_MSG(last_ack()->miss_mask == 0x02, "缺的应是第 1 片，miss_mask=%02X",
              (unsigned)last_ack()->miss_mask);
    CHECK_MSG(s_commit_count == 0, "缺片时绝不能落屏");

    send_data(SEQ, 1, 0);
    send_commit(SEQ);
    CHECK_MSG(last_ack()->sta == CASC_ACK_OK, "补片后应回 OK");
    CHECK_MSG(s_commit_count == 1, "补片后应落屏一次");
    CHECK_MSG(memcmp(s_commit_bm, s_bmp, BMP_LEN) == 0, "补片后的内容仍应与发出去的一致");
}

/** 陈旧分片：上一轮的迟到分片必须被丢弃，绝不能写进本轮暂存 */
static void case_stale_seq_dropped(void)
{
    TEST_BEGIN("陈旧分片按 seq 丢弃（CRC 是好的，只能靠 seq 拦）");

    fixture_reset();
    send_begin(SEQ, 4);
    /* 上一轮（seq-1）的分片迟到。帧头完好、CRC 也对 —— 探针会正常放行 */
    for (uint8_t k = 0; k < FRAG_N; k++) send_data((uint16_t)(SEQ - 1), k, 0);

    CHECK_MSG(s_rx.have_mask == 0, "陈旧分片被写进了本轮暂存，have_mask=%02X", s_rx.have_mask);

    send_commit(SEQ);
    CHECK_MSG(last_ack()->sta == CASC_ACK_MISS, "陈旧分片不算数，应回 MISS");
    CHECK_MSG(last_ack()->miss_mask == 0x07, "三片都该算缺，得到 %02X",
              (unsigned)last_ack()->miss_mask);

    /* 旧帧头的 frag_n 也要拦：主卡换了分片方案后旧帧会带着旧 frag_n 进来 */
    send_begin(SEQ, 4);
    send_data(SEQ, 0, 0);
    uint8_t        f[CASC_FRAME_MAX];
    const uint16_t n = build(f, CASC_T_SYNC_DATA, SEQ, 1, (uint8_t)(FRAG_N + 1), s_frag[1],
                             FRAG_BYTES);
    feed(f, n);
    send_commit(SEQ);
    CHECK_MSG(last_ack()->miss_mask == 0x06, "frag_n 不符的分片也该丢，得到 %02X",
              (unsigned)last_ack()->miss_mask);
}

/** 短分片：长度不是这一片该有的字节数，必须整片拒绝 */
static void case_short_fragment_rejected(void)
{
    TEST_BEGIN("短分片被整片拒绝（否则旧字节会永久留在屏上）");

    fixture_reset();
    send_begin(SEQ, 4);
    send_data(SEQ, 0, (uint16_t)(FRAG_BYTES - 1)); /* 短一字节 */
    send_data(SEQ, 1, 0);
    send_data(SEQ, 2, 0);
    send_commit(SEQ);

    CHECK_MSG(last_ack()->sta == CASC_ACK_MISS, "短分片不该被接受");
    CHECK_MSG(last_ack()->miss_mask == 0x01, "只有第 0 片算缺，得到 %02X",
              (unsigned)last_ack()->miss_mask);
    CHECK_MSG(s_commit_count == 0, "缺片时绝不能落屏");

    /* 末片比 frag_bytes 短是**正常**的（bmp_len 不是 frag_bytes 的整数倍），
       上面那轮已经证明了这一点：第 2 片只有 16 字节却算收到了。 */
    send_data(SEQ, 0, 0);
    send_commit(SEQ);
    CHECK_MSG(last_ack()->sta == CASC_ACK_OK, "补上完整的第 0 片后应 OK");
}

/** 几何不符：明确回 NACK，而不是将就出一幅错位画面 */
static void case_geometry_nack(void)
{
    TEST_BEGIN("矩形尺寸与本卡屏不符 → NACK，且不进入本轮");

    fixture_reset();
    const casc_sync_begin_t bad =
        begin_payload((uint16_t)(DEV_W + 1), DEV_H, BMP_LEN, FRAG_BYTES, FRAG_N, 4, COLOR_GREEN);
    uint8_t        f[64];
    const uint16_t n = build(f, CASC_T_SYNC_BEGIN, SEQ, 0, 0, &bad, sizeof(bad));
    feed(f, n);

    CHECK_MSG(last_is(CASC_T_NACK), "几何不符应回 NACK 而不是静默丢");
    CHECK_MSG(s_tx_frame[11] == CASC_NACK_GEOM, "NACK 原因应为 GEOM，得到 %u",
              (unsigned)s_tx_frame[11]);
    CHECK_MSG(!s_rx.active, "NACK 之后不该处于本轮激活状态");

    /* 主卡若仍发 COMMIT，从卡应说"我没有这一轮"，让主卡整轮重来 */
    send_commit(SEQ);
    CHECK_MSG(last_ack()->sta == CASC_ACK_NOBEGIN, "未 BEGIN 就 COMMIT 应回 NOBEGIN");
    CHECK_MSG(s_commit_count == 0, "NOBEGIN 时绝不能落屏");
}

/** 重复 COMMIT 幂等：主卡没收到 ACK 会重发 COMMIT，不该被判成"整轮白做" */
static void case_double_commit_idempotent(void)
{
    TEST_BEGIN("重复 COMMIT 幂等（ACK 丢了主卡会重发 COMMIT）");

    fixture_reset();
    send_begin(SEQ, 4);
    send_all_data(SEQ);
    send_commit(SEQ);
    send_commit(SEQ);

    CHECK_MSG(s_commit_count == 2, "两次 COMMIT 应各落屏一次（内容相同，无副作用）");
    CHECK_MSG(last_ack()->sta == CASC_ACK_OK, "第二次 COMMIT 仍应回 OK，得到 sta=%u",
              (unsigned)last_ack()->sta);
}

/** ABORT：主卡放弃本轮，暂存必须清掉 */
static void case_abort_clears(void)
{
    TEST_BEGIN("ABORT 清掉暂存，其后的 COMMIT 不再落屏");

    fixture_reset();
    send_begin(SEQ, 4);
    send_data(SEQ, 0, 0);
    send_abort(SEQ);

    CHECK_MSG(!s_rx.active, "ABORT 之后本轮应失效");

    send_commit(SEQ);
    CHECK_MSG(last_ack()->sta == CASC_ACK_NOBEGIN, "ABORT 后 COMMIT 应回 NOBEGIN");
    CHECK_MSG(s_commit_count == 0, "ABORT 后绝不能落屏");
}

/** 亮度每轮重新断言 —— "从卡永久停在旧亮度" 的唯一防线 */
static void case_bright_asserted(void)
{
    TEST_BEGIN("每轮 BEGIN 都重新断言亮度");

    fixture_reset();
    send_begin(SEQ, 5);
    CHECK_MSG(s_bright_calls == 1 && s_bright_last == 5, "BEGIN 应把亮度断言到 5，得到 %u",
              (unsigned)s_bright_last);

    /* 下一轮改亮度，必须跟着变（SET_BRIGHT 广播丢了就靠这里自愈） */
    fixture_reset();
    send_begin(SEQ, 2);
    CHECK_MSG(s_bright_calls == 1 && s_bright_last == 2, "下一轮应重新断言成 2，得到 %u",
              (unsigned)s_bright_last);

    /* 亮度断言在**参数校验之后**：几何不符时不改屏上任何东西 */
    fixture_reset();
    const casc_sync_begin_t bad =
        begin_payload((uint16_t)(DEV_W + 1), DEV_H, BMP_LEN, FRAG_BYTES, FRAG_N, 6, COLOR_GREEN);
    uint8_t        f[64];
    const uint16_t n = build(f, CASC_T_SYNC_BEGIN, SEQ, 0, 0, &bad, sizeof(bad));
    feed(f, n);
    CHECK_MSG(s_bright_calls == 0, "被 NACK 的 BEGIN 不该改亮度");
}

/** 主卡收到发给从卡的帧：什么都不做（半双工回声 / 地址配重时会遇到） */
static void case_master_ignores(void)
{
    TEST_BEGIN("本卡是主卡时，图传命令一律不应答、不落屏");

    fixture_reset();
    s_is_master = true;

    send_begin(SEQ, 4);
    send_all_data(SEQ);
    send_commit(SEQ);

    CHECK_MSG(s_tx_count == 0, "主卡不该应答发给从卡的帧，却发了 %d 帧", s_tx_count);
    CHECK_MSG(s_commit_count == 0, "主卡不该按从卡路径落屏");
    CHECK_MSG(!s_rx.active, "主卡不该进入本轮激活状态");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36m级联图传 · 从卡侧（本卡 %ux%u）\033[0m\n", (unsigned)DEV_W, (unsigned)DEV_H);

    case_round_ok();
    case_missing_fragment();
    case_stale_seq_dropped();
    case_short_fragment_rejected();
    case_geometry_nack();
    case_double_commit_idempotent();
    case_abort_clears();
    case_bright_asserted();
    case_master_ignores();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
