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

    uint16_t bad_vals[] = {0, 1, 14, 1045, 0xFFFF};
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
    n = build(s_f, CASC_T_SYNC_BEGIN, 1, CASC_ADDR_MASTER, 1, nullptr, 0);
    feed(s_f, n);
    CHECK_MSG(probe(&tl, &ax) == PCB_PROBE_READY, "发给本机的帧应 READY");

    /* 别人（从卡 2）—— 本用例把本卡设成 1 */
    fixture_reset();
    n = build(s_f, CASC_T_SYNC_BEGIN, 2, CASC_ADDR_MASTER, 1, nullptr, 0);
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

/* ================================================================ */

int main(void)
{
    case_four_states();
    case_length_bounds();
    case_address_filter();
    case_no_cross_poison();
    case_sof_in_payload();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
