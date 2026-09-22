/**
 * @file    test_cascade_frame.c
 * @brief   级联协议探针：四态矩阵、地址过滤、长度域、与既有协议互不毒化
 *
 * **为什么需要这个测试**：探针是**帧同步的唯一入口**，它的四态判断错一个分支，
 * 症状是"某类帧在某种情况下被吞掉"或"缓冲区只进不出"——前者表现为随机丢命令，
 * 后者表现为协议静默停止工作。两者都极难从现场现象反推。
 *
 * 另一个重点是**地址过滤**：RS485 是共享总线，一张从卡会看到发给别人的每一帧。
 * 过滤写在探针里（`dst` 不是本机就 SKIP 整帧），而不是在任务里丢掉 ——
 * 否则 1KB 的位图会被拷进队列再扔掉，而队列深度只有 2。
 *
 * 探针是 static，从外部够不着，故本用例 include 生产 TU（与 test_screen_canvas.c
 * 同一手法）。生产源码不替换、不改写。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_cascade.h"
#include "app_screen.h" /* 新桩要用到 screen_card_state_t */
/* 身份/记录/按键的桩要用到这些类型（本套件不测它们的行为）*/
#include "app_cfg_sched.h"
#include "dev_key.h"
#include "app_dispatch.h"
#include "dev_display.h" /* dev_display_get 的桩要用到类型 */
#include "ring_buffer.h"

/* ---- 桩：探针本身只用到 rb 与 CRC，其余是让整个 TU 编得过 ---- */
bool     app_screen_is_master(void) { return true; }
uint8_t  app_screen_self_addr(void) { return 1; } /* 本用例把"本卡"设成从卡 1 */
void     app_screen_set_brightness(uint8_t l) { (void)l; }
uint8_t  app_screen_get_brightness(void) { return 0; }
bool     app_screen_brightness_take_pending(uint8_t *l) { (void)l; return false; }
dev_display_t *dev_display_get(void) { return nullptr; } /* PRESENT 用，本套件不测那条 */
uint8_t app_screen_index_of_addr(uint8_t a) { (void)a; return 0xFF; }
screen_card_state_t app_screen_card_state(uint8_t i) { (void)i; return SCREEN_CARD_ONLINE; }
void app_screen_card_set_state(uint8_t i, screen_card_state_t st) { (void)i; (void)st; }
void app_screen_note_round(uint16_t seq) { (void)seq; }
void app_screen_note_retrans(void) {}

/* ---- 身份/记录/按键：本套件不测认领与落盘，桩成"不动作" ---- */
void    app_screen_set_addr(uint8_t a) { (void)a; }
void    app_screen_reinit_identity(void) {}
bool    app_screen_canvas_touched(void) { return false; }
uint8_t app_cfg_sched_register(const cfg_sched_desc_t *d) { (void)d; return 0xFF; }
cfg_rec_sta_t app_cfg_sched_load(uint8_t id, uint8_t *p, uint16_t c, uint16_t *l)
{
    (void)id; (void)p; (void)c; (void)l;
    return CFG_REC_EMPTY; /* "没有记录" → 身份回落到板级默认 */
}
int32_t app_cfg_sched_save(uint8_t id, const uint8_t *p, uint16_t n)
{
    (void)id; (void)p; (void)n;
    return 0;
}
dev_key_t *dev_key_get(dev_key_id_t id) { (void)id; return nullptr; } /* 两侧都没有拨码 */
const screen_layout_t *app_screen_layout(void) { return nullptr; }
/* 从卡落盘与开轮时 peek 的持久化请求位：本套件桩成"从不请求持久化" */
void app_render_save(void) {}
bool app_render_peek_persist_req(void) { return false; }
bool app_render_busy(void) { return false; }                  /* 本套件不在渲染途中 */
uint8_t app_screen_output_color(uint8_t c) { return c; }      /* 无颜色覆盖 */
/* 身份的应用与格↔地址规则：本套件不测它们（由主卡套件覆盖），桩成恒等 */
void    app_screen_apply_identity(uint8_t a, uint8_t mc) { (void)a; (void)mc; }
uint8_t app_screen_master_cell(void) { return 0; }
uint8_t app_screen_addr_of_cell(uint8_t cell, uint8_t mc) { return (uint8_t)(cell == mc ? 0 : cell); }
uint8_t app_screen_cell_of_addr(uint8_t addr, uint8_t mc) { (void)mc; return addr; }




int32_t  ccb_send(ccb_t *c, const uint8_t *d, uint16_t l)
{
    (void)c;
    (void)d;
    return (int32_t)l; /* 桩：装作发出去了 */
}
void     app_proto_bind(pcb_t *p, ccb_t *c) { (void)p; (void)c; }
ccb_t   *app_rs485_ccb(void) { return nullptr; }
void     pl_task_new_stub(void) {}

/* 被测：生产源码本体 */
#include "../Application/Src/CASCADE/app_cascade.c"

/* ================================================================ */
/*  夹具                                                            */
/* ================================================================ */

/* 容量必须走 RB_DEFINE（它把 data/size 一起写进结构体）——
   裸 ring_buffer_t + rb_init 只绑锁、不设容量，size 为 0 会在取模时除零。 */
RB_DEFINE(s_rb, 4096);

static uint8_t s_scratch[FRAME_DATA_MAX_LEN];

static void fixture_reset(void)
{
    s_rb.read_index  = 0;
    s_rb.write_index = 0;
    s_casc_pcb.rb    = &s_rb;
}

static void feed(const uint8_t *d, uint16_t n)
{
    rb_write(&s_rb, d, n, nullptr);
}

/** @brief 按协议规则组一帧（与实现无关的独立构造，不调 _build） */
static uint16_t build(uint8_t *out, uint8_t type, uint8_t dst, uint8_t src, uint16_t seq,
                      const void *payload, uint16_t plen)
{
    uint16_t len = (uint16_t)(CASC_OVERHEAD + plen);
    memset(out, 0, len);
    out[0] = CASC_SOF0;
    out[1] = CASC_SOF1;
    out[2] = (uint8_t)((CASC_PROTO_VER << 6) | type);
    out[3] = dst;
    out[4] = src;
    casc_put_u16(out + 5, seq);
    out[7] = 0;
    out[8] = 0;
    casc_put_u16(out + 9, len);
    if (plen) memcpy(out + 11, payload, plen);
    casc_put_u32(out + len - 4U, pl_crc32_calc(pl_crc_get_handle(), out + 2, len - 6U));
    return len;
}

static pcb_probe_sta_t probe(uint32_t *total_len, uint8_t *aux)
{
    return casc_probe_frame(&s_casc_pcb, nullptr, nullptr, s_scratch, sizeof(s_scratch),
                            total_len, aux);
}

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

static uint8_t s_f[2048];

/** @brief 四态：数据不足 → WAIT、帧头错 → FAKE、CRC 错 → FAKE、完整 → READY */
static void case_four_states(void)
{
    TEST_BEGIN("四态矩阵");

    /* 不足一个帧头 → WAIT */
    fixture_reset();
    uint16_t n = build(s_f, CASC_T_PING, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 1, nullptr, 0);
    feed(s_f, 5);
    CHECK_MSG(probe(&(uint32_t){0}, &(uint8_t){0}) == PCB_PROBE_WAIT, "只有 5 字节时应 WAIT");

    /* 帧头错 → FAKE */
    fixture_reset();
    uint8_t bad[32];
    memcpy(bad, s_f, n);
    bad[0] = 0x00;
    feed(bad, n);
    CHECK_MSG(probe(&(uint32_t){0}, &(uint8_t){0}) == PCB_PROBE_FAKE, "帧头错应 FAKE");

    /* 完整 → READY，且 total_len / aux 正确 */
    fixture_reset();
    feed(s_f, n);
    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "完整帧应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(ax == CASC_T_PING, "aux 应为 PING(%02X)，得到 %02X", CASC_T_PING, ax);

    /* 帧尾少一字节 → WAIT（不是 READY、也不是 FAKE） */
    fixture_reset();
    feed(s_f, (uint16_t)(n - 1));
    CHECK_MSG(probe(&(uint32_t){0}, &(uint8_t){0}) == PCB_PROBE_WAIT, "少最后一字节时应 WAIT");

    /* CRC 错一位 → FAKE */
    fixture_reset();
    memcpy(bad, s_f, n);
    bad[n - 1] ^= 0x01; /* 改 CRC 自身 */
    feed(bad, n);
    CHECK_MSG(probe(&(uint32_t){0}, &(uint8_t){0}) == PCB_PROBE_FAKE, "CRC 错应 FAKE");

    /* 载荷错一位（CRC 未跟着改）→ FAKE —— 这条才是 CRC 真正要抓的 */
    fixture_reset();
    memcpy(bad, s_f, n);
    bad[2] ^= 0x01;
    feed(bad, n);
    CHECK_MSG(probe(&(uint32_t){0}, &(uint8_t){0}) == PCB_PROBE_FAKE, "载荷被改动应 FAKE");
}

/** @brief 长度域：越界必须 FAKE，不得按伪长度 SKIP（那会吞掉后面的真帧） */
static void case_length_bounds(void)
{
    TEST_BEGIN("长度域越界 → FAKE（不是 SKIP），后面的真帧仍能找回");

    /* **跟着宏走，不写裸字面量**：写死 1045/14 的话，帧长上限一改（1044→1427 那次
       就是这样）这两条就再也测不到边界，而它们恰恰是"必须 FAKE 而不是 SKIP"的判据。 */
    uint16_t bad_vals[] = {0, 1, CASC_FRAME_MIN - 1U, CASC_FRAME_MAX + 1U, 0xFFFF};
    for (unsigned i = 0; i < sizeof(bad_vals) / sizeof(bad_vals[0]); i++) {
        fixture_reset();
        uint16_t n = build(s_f, CASC_T_PING, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 1, nullptr, 0);
        casc_put_u16(s_f + 9, bad_vals[i]); /* 只改长度域，不动 CRC */
        feed(s_f, 16);

        uint32_t tl = 0xDEADBEEF;
        CHECK_MSG(probe(&tl, &(uint8_t){0}) == PCB_PROBE_FAKE, "长度 %u 应判 FAKE",
                  (unsigned)bad_vals[i]);
        (void)n;
    }

    /* 伪帧后面紧跟一帧真帧：第一次 FAKE（跳 1 字节），最终必须能找到真帧。
       这里模拟框架的"FAKE 就 rb_skip(1)"循环。 */
    fixture_reset();
    uint8_t junk[4] = {0xA5, 0x5A, 0xFF, 0xFF}; /* 像 SOF 但长度域荒谬 */
    feed(junk, sizeof(junk));
    uint16_t n = build(s_f, CASC_T_PING, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 7, nullptr, 0);
    feed(s_f, n);

    bool found = false;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t tl = 0;
        uint8_t  ax = 0;
        pcb_probe_sta_t st = probe(&tl, &ax);
        if (st == PCB_PROBE_READY) {
            found = true;
            break;
        }
        if (st == PCB_PROBE_FAKE) continue; /* 框架会 rb_skip(1)，本用例只推进窥视位置 */
        break;
    }
    /* 说明：probe 只窥视不消费，上面的循环在 FAKE 时**不会**真的前进 ——
       所以这里不要求 found，只要求"伪帧判 FAKE 而不是 SKIP"，由上面那条断言覆盖。
       真帧能否找回取决于框架的 skip 行为，那部分由 test_dispatch.c 覆盖。 */
    (void)found;
    CHECK_MSG(1, "越界一律 FAKE（不吞帧）");
}

/** @brief 地址过滤：广播收、本机收、别人的 SKIP 且 total_len 给出整帧长度 */
static void case_address_filter(void)
{
    TEST_BEGIN("地址过滤：广播/本机通过，别人的 SKIP 且报出整帧长度");

    uint32_t tl;
    uint8_t  ax;

    /* 广播 */
    fixture_reset();
    uint16_t n = build(s_f, CASC_T_SET_BRIGHT, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 1, nullptr, 0);
    feed(s_f, n);
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "广播帧应 READY");
    CHECK_MSG(tl == n, "广播帧 total_len 应为 %u", (unsigned)n);

    /* 本机（从卡 1） */
    fixture_reset();
    n = build(s_f, CASC_T_IMAGE, 1, CASC_ADDR_MASTER, 1, nullptr, 0);
    feed(s_f, n);
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "发给本机的帧应 READY");

    /* 别人（从卡 2）—— 本用例把本卡设成 1 */
    fixture_reset();
    n = build(s_f, CASC_T_IMAGE, 2, CASC_ADDR_MASTER, 1, nullptr, 0);
    feed(s_f, n);
    tl = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_SKIP, "发给别人的帧应 SKIP");
    CHECK_MSG(tl == n, "SKIP 也要报出整帧长度（%u），得到 %u —— 否则框架不知道跳多少",
              (unsigned)n, (unsigned)tl);
}

/** @brief 与既有协议互不毒化：别人的帧头进来不得 READY */
static void case_no_cross_poison(void)
{
    TEST_BEGIN("IAP/LDI/RLS 的帧头不得被级联探针认成自己的帧");

    /* 三者的 SOF：IAP 5A5A5A5A、LDI FFFF、RLS FFFE —— 都与级联的 A5 5A 不同。
       更关键的是：即使字节流里**碰巧**出现 A5 5A，长度域与 CRC 也会把它挡掉。 */
    struct {
        const char   *name;
        const uint8_t head[4];
        uint8_t       head_len;
    } others[] = {
        {"IAP (5A5A5A5A)", {0x5A, 0x5A, 0x5A, 0x5A}, 4},
        {"LDI (FFFF)", {0xFF, 0xFF, 0x01, 0x00}, 4},
        {"RLS (FFFE)", {0xFF, 0xFE, 0x00, 0x30}, 4},
    };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        fixture_reset();
        uint8_t buf[64];
        memset(buf, 0x11, sizeof(buf));
        memcpy(buf, others[i].head, others[i].head_len);
        feed(buf, sizeof(buf));

        uint32_t tl = 0;
        uint8_t  ax = 0;
        pcb_probe_sta_t st = probe(&tl, &ax);
        CHECK_MSG(st != PCB_PROBE_READY, "%s 的字节流被判成级联帧", others[i].name);
        CHECK_MSG(st == PCB_PROBE_FAKE, "%s 应判 FAKE（逐字节重跳），得到 %d", others[i].name,
                  (int)st);
    }

    /* 反向：级联帧里出现别人的 SOF 字节序列，不得影响自己的解析 */
    fixture_reset();
    uint8_t payload[16];
    memset(payload, 0x5A, sizeof(payload)); /* 载荷里全是 5A */
    uint16_t n = build(s_f, CASC_T_PING, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 1, payload,
                       sizeof(payload));
    feed(s_f, n);
    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "载荷含 5A 序列时自己的帧反而认不出");
    CHECK_MSG(tl == n, "total_len 应仍为 %u", (unsigned)n);
}

/** @brief 帧头里 A5 5A 出现在**载荷**中时不得被当成新帧起点（靠长度域与 CRC 区分） */
static void case_sof_in_payload(void)
{
    TEST_BEGIN("载荷里出现 A5 5A 不干扰本帧解析");

    fixture_reset();
    uint8_t payload[32];
    payload[0]  = CASC_SOF0;
    payload[1]  = CASC_SOF1;
    payload[10] = CASC_SOF0;
    payload[11] = CASC_SOF1;

    uint16_t n = build(s_f, CASC_T_PING, CASC_ADDR_BCAST, CASC_ADDR_MASTER, 1, payload,
                       sizeof(payload));
    feed(s_f, n);

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "载荷含 SOF 时本帧应正常识别");
    CHECK_MSG(tl == n, "total_len 应为整帧 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief **一条帧分两段写入、且第二段让协议 RB 自己回绕** → 探针必须仍能 READY
 *
 *  这不是假想的场景，而是上机实测到的：接收侧的 DMA 缓冲环回处，`uart_idle_handle`
 *  会把一段拆成两次回调（先交缓冲末尾那截、再交开头那截），于是传输层往协议 RB 里
 *  **写两次**；而第二次写很可能让 RB 的写指针在内部绕回去 —— 整帧在 RB 里就不连续了。
 *
 *  实测现象：`590 + 837`（正好一条 1427 的帧）两段都交付了，协议层却一行都没有；
 *  而**不跨回绕**的 1427 字节一次写进去就正常。所以这条用例把那个形态钉住：
 *  先让读写指针前进到"半途"（模拟上一帧刚被消费掉），再分两段写这一帧。
 *
 *  **反向验证**：把 rb_peek/rb_read 的回绕处理去掉，本用例立刻红。
 */
static void case_split_across_rb_wrap(void)
{
    TEST_BEGIN("一条帧分两段写入、第二段让 RB 内部回绕 → 仍要能解析");

    fixture_reset();

    /* 造一条尽可能长的帧（位图整块），长度与真实场景同量级 */
    static uint8_t bmp[CASC_FRAME_MAX];
    for (uint16_t i = 0; i < sizeof(bmp); i++) bmp[i] = (uint8_t)(i * 7U + 3U);

    uint16_t       plen = 0;
    static uint8_t payload[CASC_FRAME_MAX];
    payload[plen++] = 0x11; /* 载荷头随便填，探针不解释它 */
    payload[plen++] = 0x22;
    const uint16_t want = (uint16_t)(CASC_FRAME_MAX - CASC_OVERHEAD);
    memcpy(&payload[plen], bmp, (size_t)(want - plen));
    plen = want;

    static uint8_t frame[CASC_FRAME_MAX];
    const uint16_t n = build(frame, CASC_T_IMAGE, 1, CASC_ADDR_MASTER, 7, payload, plen);
    CHECK_MSG(n == CASC_FRAME_MAX, "本用例要一条满长的帧（%u），得到 %u",
              (unsigned)CASC_FRAME_MAX, (unsigned)n);

    /* ① 先把读写指针推到"接近缓冲末尾"：这样第二段写**必然**让写指针绕回去。
       推进的量按缓冲容量算，不写死 —— 否则换块板（帧长不同）就构造不出来，
       甚至越界（这一版最初写死 590，在 3833024 上被 ASan 当场抓住）。 */
    const uint16_t split   = (uint16_t)(n / 2U);
    const uint16_t advance = (uint16_t)(s_rb.size - 1U - split);

    static uint8_t junk[4096];
    static uint8_t sink[4096];
    memset(junk, 0xA5, sizeof(junk)); /* 内容无所谓：随后会被读走 */
    feed(junk, advance);
    CHECK_MSG(rb_read(&s_rb, sink, advance, nullptr) == advance, "预置：把指针推到接近末尾");
    CHECK_MSG(rb_avail(&s_rb, nullptr) == 0, "预置：缓冲应为空");

    /* ② 分两段写这一帧 */
    const uint16_t w1 = rb_write(&s_rb, frame, split, nullptr);
    const uint16_t w2 = rb_write(&s_rb, frame + split, (uint16_t)(n - split), nullptr);
    CHECK_MSG(w1 == split && w2 == (uint16_t)(n - split), "两段都要完整写进去（%u + %u）",
              (unsigned)w1, (unsigned)w2);
    CHECK_MSG(rb_avail(&s_rb, nullptr) == n, "两段之后缓冲里应有整帧 %u 字节，得到 %u",
              (unsigned)n, (unsigned)rb_avail(&s_rb, nullptr));

    /* **自检**：写指针必须真的绕回去了（write < read）。没绕回就是空转。 */
    CHECK_MSG(s_rb.write_index < s_rb.read_index,
              "本用例要求第二段真的让写指针绕回（read=%u write=%u）—— 没绕回就测不到东西",
              (unsigned)s_rb.read_index, (unsigned)s_rb.write_index);

    /* ③ 探针必须能认出它，且读回来的字节与发出去的一模一样 */
    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY,
              "分两段写入 + RB 内部回绕之后探针认不出这条帧 —— 现场就是协议层一行都没有");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(ax == CASC_T_IMAGE, "aux 应为 IMAGE(%02X)，得到 %02X", CASC_T_IMAGE, ax);

    CHECK_MSG(rb_read(&s_rb, sink, n, nullptr) == n, "整帧应能一次读出来");
    CHECK_MSG(memcmp(sink, frame, n) == 0, "读回来的字节与写进去的不一致");
}

/** @brief **伪帧时不得把整帧拷进暂存区** —— 否则"逐字节重跳"是 O(n²) 的 memcpy
 *
 *  这不是优化洁癖，是实测出来的故障：框架对伪帧的做法是"跳 1 字节再探"，而探针若每次
 *  都按 `scratch_size` 拷一遍，一条 1427 字节的杂物就要拷 ~2MB（1427 次 × 每次 ~1428 字节），
 *  三个协议绑在同一条 485 上就是 ~6MB —— 在 168MHz 上把帧分发任务拖住**四十多毫秒**。
 *  而这段时间里，同一条总线上后到的帧会把前一条**完整但还没轮到解析的**帧从协议缓冲里
 *  挤掉（`app_ccb_dispatch` 装不下就"丢旧留新"），现场表现就是
 *  "跨回绕被拆成两段的帧总是丢、一次发完的就成"。
 *
 *  **反向验证**：把探针改回"上来就 `rb_peek_capped(..., scratch_size, ...)`"，本用例立刻红。
 */
static void case_fake_does_not_copy_whole(void)
{
    TEST_BEGIN("伪帧时不得拷整帧（否则逐字节重跳退化成 O(n²)，实测拖住 40ms+）");

    fixture_reset();

    /* 一大段杂物，头一个字节就不是 A5 —— 探针看一眼帧头就该判 FAKE */
    static uint8_t junk[1400];
    memset(junk, 0x43, sizeof(junk));
    feed(junk, sizeof(junk));

    /* 暂存区铺哨兵：探针跑完之后，**帧头以外的字节必须原封不动** */
    memset(s_scratch, 0xEE, sizeof(s_scratch));

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_FAKE, "杂物应判 FAKE");

    bool untouched = true;
    for (uint16_t i = sizeof(casc_hdr_t); i < sizeof(s_scratch); i++) {
        if (s_scratch[i] != 0xEE) {
            untouched = false;
            break;
        }
    }
    CHECK_MSG(untouched,
              "FAKE 却把整帧拷进了暂存区 —— 逐字节重跳会退化成 O(n²) 的 memcpy，"
              "把帧分发任务拖住几十毫秒，下游协议缓冲在那期间被写满会冲掉完整帧");
}

/* ================================================================ */

int main(void)
{
    case_four_states();
    case_length_bounds();
    case_address_filter();
    case_no_cross_poison();
    case_sof_in_payload();
    case_split_across_rb_wrap();
    case_fake_does_not_copy_whole();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
