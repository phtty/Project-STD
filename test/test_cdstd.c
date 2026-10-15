/**
 * @file    test_cdstd.c
 * @brief   CDSTD（折叠屏创迪标准协议）host 单测：探针四态 / 整帧等待 / CRC→FAKE、
 *          长度域与暂存区契约、跨协议互不毒化（IAP/CASC/RLS）、队列取帧与命令分派
 *
 * **为什么需要这个测试**：探针是帧同步的唯一入口（同 test_casc_frame 的论证），
 * 判错一个分支的症状是"某类帧被吞掉"或"缓冲区只进不出"，都极难从现场反推。
 * 本协议另有三处**骨架落地时修复的控制流**，各由一条用例钉住：
 *   1. 队列取帧判断反了（真帧被丢、反拿未初始化的 msg 去查表）—— case_task_dispatch；
 *   2. 整帧到齐之前就比 CRC（分段到达的帧被当坏帧丢）—— case_wait_whole_frame；
 *   3. CRC 不过返回 SKIP（按伪长度把后面的真帧一起吞掉）—— case_crc_fail_is_fake。
 *
 * 另一重点是**互不毒化**：其它协议（IAP/CASC/RLS）的字节流不得被判成 CDSTD 帧；
 * （LDI 与 CDSTD 帧几何同构、帧层面不可区分，但两者不会同时启用，故不做该断言，见用例内注释。）
 *
 * 手法：用例 TU-include 生产 TU（探针是 non-static、任务与模块资源都是 static，
 * 外部够不着），**命令表整个换成记录桩**——生产表当前全是空桩，没有可观察行为；
 * 本套件要测的是任务侧的取帧与分派机制。任务经真实 pl_task_new 跑在 pthread 上
 * （与 test_dispatch 同一手法）。
 *
 * 帧由用例独立构造（不复用探针的中间量），但按**帧结构体的字段布局**写入 ——
 * 结构体即线格式契约（与 test_probes 同口径）；CRC 尾按大端存放，与实现的读取一致。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_cdstd.h"
#include "app_cdstd_cmd.h"
#include "app_dispatch.h"
#include "crc_utils.h"
#include "ring_buffer.h"

/* ================================================================
 *  桩：模块 init 会碰到的通道访问器与绑定
 * ================================================================ */

static int s_bind_calls;

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
    s_bind_calls++;
}

app_ccb_t *app_udp_ccb(void) { return nullptr; }
app_ccb_t *app_rs485_ccb(void) { return nullptr; }

/* ================================================================
 *  命令表：记录桩
 *
 *  生产表（app_cdstd_cmd.c）当前全是空处理桩，观察不到"分派发生了没有"；
 *  本套件换成记录型替身，验证任务把 msg->aux 当命令号、按帧内长度域取 data_len
 *  并原样传给处理函数。表长与枚举绑定（少一项就是任务越界索引）。
 * ================================================================ */

static int        s_calls[CDSTD_CMD_COUNT];
static uint16_t   s_last_len[CDSTD_CMD_COUNT];
static app_ccb_t *s_last_ccb[CDSTD_CMD_COUNT];

#define CDSTD_REC_HANDLER(n)                                               \
    static void _rec_##n(app_ccb_t *ccb, void *data, uint16_t data_len)    \
    {                                                                      \
        (void)data;                                                        \
        s_calls[n]++;                                                      \
        s_last_len[n] = data_len;                                          \
        s_last_ccb[n] = ccb;                                               \
    }

CDSTD_REC_HANDLER(0)
CDSTD_REC_HANDLER(1)
CDSTD_REC_HANDLER(2)
CDSTD_REC_HANDLER(3)
CDSTD_REC_HANDLER(4)
CDSTD_REC_HANDLER(5)
CDSTD_REC_HANDLER(6)
CDSTD_REC_HANDLER(7)
CDSTD_REC_HANDLER(8)

const app_cdstd_cmd_handler_fn_t g_cdstd_cmd_table[] = {
    _rec_0, _rec_1, _rec_2, _rec_3, _rec_4, _rec_5, _rec_6, _rec_7, _rec_8,
};

static_assert(sizeof(g_cdstd_cmd_table) / sizeof(g_cdstd_cmd_table[0]) == CDSTD_CMD_COUNT,
              "记录桩表必须与 CDSTD_CMD_COUNT 一一对应");

/* ================================================================ */
/*  被测：生产源码本体（与 test_casc_frame 同一手法）               */
/* ================================================================ */

#include "../Application/Src/CDSTD/app_cdstd.c"

/* ================================================================ */
/*  夹具                                                            */
/* ================================================================ */

/* 探针只用 self->rb；本套件自建大缓冲 —— 1400 字节杂物、回绕用例都要地方，
   生产的 1024 缓冲放不下这些场景。容量必须走 RB_DEFINE（裸 ring_buffer_t +
   rb_init 只绑锁、不设容量，size 为 0 会在取模时除零）。 */
RB_DEFINE(s_ut_rb, 4096);

static uint8_t s_frame[2048];
__attribute__((aligned(4))) static uint8_t s_scratch_buf[FRAME_DATA_MAX_LEN];

static void fixture_reset(void)
{
    s_ut_rb.read_index  = 0;
    s_ut_rb.write_index = 0;
    s_cdstd_pcb.rb      = &s_ut_rb;
}

static void cdstd_feed(const uint8_t *d, uint16_t n)
{
    rb_write(&s_ut_rb, d, n, nullptr);
}

static app_pcb_probe_state_t cdstd_probe(uint32_t *total_len, uint8_t *aux, uint16_t scratch_cap)
{
    return app_cdstd_probe_frame(&s_cdstd_pcb, nullptr, nullptr, s_scratch_buf, scratch_cap,
                                 total_len, aux);
}

/**
 * @brief 按帧结构体组一帧（独立构造，不复用探针中间量）
 *
 * LEN 字段 = plen + 1（含 CMD_TYPE，文档口径）；CRC16-XMODEM 覆盖 ver ~ DATA 尾
 * （长度 = plen + 7 = 6 + LEN），大端落在 DATA 之后 —— 与探针的读法一致。
 * 帧头/版本/序号对本套件无意义，填固定值。
 */
static uint16_t cdstd_build(uint8_t *out, uint8_t cmd, uint16_t plen, const void *payload)
{
    app_cdstd_frame_t *f = (app_cdstd_frame_t *)out;
    memset(out, 0, (size_t)sizeof(app_cdstd_frame_t) + plen + 2U);

    f->head[0] = 0xFF;
    f->head[1] = 0xFF;
    f->ver     = 0x01;
    f->seq     = 0x00;
    f->len[0]  = (uint8_t)((plen + 1U) >> 8);
    f->len[1]  = (uint8_t)(plen + 1U);
    f->cmd     = cmd;
    if (payload != nullptr && plen > 0) memcpy(&f->data_crc[0], payload, plen);

    uint16_t crc = crc16_xmodem(out + 2, (size_t)plen + sizeof(app_cdstd_frame_t) - 2U);
    f->data_crc[plen]     = (uint8_t)(crc >> 8);
    f->data_crc[plen + 1] = (uint8_t)(crc & 0xFF);

    return (uint16_t)(sizeof(app_cdstd_frame_t) + plen + 2U);
}

/* ================================================================
 *  断言与统计
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                 \
            printf("\n");                                                        \
        }                                                                        \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================ */

/** @brief 四态：不足 → WAIT、帧头错 → FAKE、完整 → READY、CRC 错 → FAKE */
static void case_probe_states(void)
{
    TEST_BEGIN("探针四态：WAIT / FAKE（帧头错）/ READY / FAKE（CRC 错）");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA3, 20, nullptr);
    CHECK_MSG(n == 31, "本用例帧长应为 31，得到 %u", (unsigned)n);

    /* 不足一个帧头 → WAIT */
    fixture_reset();
    cdstd_feed(s_frame, 5);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "只有 5 字节时应 WAIT");

    /* 帧头错 → FAKE */
    fixture_reset();
    uint8_t bad[64];
    memcpy(bad, s_frame, n);
    bad[0] = 0x00;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "帧头错应 FAKE");

    /* 完整 → READY：total_len / aux / 不消费都要对 */
    fixture_reset();
    cdstd_feed(s_frame, n);
    tl = 0;
    ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "完整帧应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
    CHECK_MSG(ax == (0xA3 & 0x0F), "aux 应为命令低 4 位（%02X），得到 %02X", 0xA3 & 0x0F, ax);
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == n, "探针只窥视，不得消费");

    /* 少最后一字节 → WAIT
       **反向验证**：去掉"整帧到齐"判断，这里会按半帧比 CRC、判成 FAKE */
    fixture_reset();
    cdstd_feed(s_frame, (uint16_t)(n - 1));
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "少最后一字节时应 WAIT（分段到达不能被当坏帧丢）");

    /* CRC 错 → FAKE */
    fixture_reset();
    memcpy(bad, s_frame, n);
    bad[n - 1] ^= 0x01;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "CRC 错应 FAKE");

    /* 载荷错一位（CRC 未跟着改）→ FAKE —— 这条才是 CRC 真正要抓的 */
    fixture_reset();
    memcpy(bad, s_frame, n);
    bad[10] ^= 0x01;
    cdstd_feed(bad, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "载荷被改动应 FAKE");
}

/** @brief 分段到达：整帧到齐前必须 WAIT（不能按半帧比 CRC） */
static void case_wait_whole_frame(void)
{
    TEST_BEGIN("分段到达：整帧到齐前必须 WAIT，且不消费");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA1, 300, nullptr);
    CHECK_MSG(n == 311, "本用例帧长应为 311，得到 %u", (unsigned)n);

    fixture_reset();
    cdstd_feed(s_frame, 120); /* 帧头 + 长度域已到，整帧未到 */
    uint16_t before = rb_avail(&s_ut_rb, nullptr);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_WAIT,
              "整帧未到齐应 WAIT");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == before, "WAIT 不得消费");

    cdstd_feed(s_frame + 120, (uint16_t)(n - 120));
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "补齐后应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief 长度域：0 / 512 边界 READY，513 / 0xFFFF 越界 FAKE */
static void case_length_domain(void)
{
    TEST_BEGIN("长度域：0 / 512 边界 READY，513 / 0xFFFF 越界 FAKE");

    uint32_t tl = 0;
    uint8_t  ax = 0;

    /* 最短帧（DATA 域 0 字节）→ READY，total = 11 */
    fixture_reset();
    uint16_t n = cdstd_build(s_frame, 0xA0, 0, nullptr);
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "0 长度帧应 READY");
    CHECK_MSG(n == 11 && tl == 11, "最短帧应为 11 字节（构造 %u / 返回 %u）", (unsigned)n,
              (unsigned)tl);

    /* 最长帧（DATA 域 512）→ READY，total = 523 */
    static uint8_t big[512];
    memset(big, 0x5A, sizeof(big));
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 512, big);
    CHECK_MSG(n == 523, "最长帧应为 523 字节，得到 %u", (unsigned)n);
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "512 长度帧应 READY");
    CHECK_MSG(tl == 523, "total_len 应为 523，得到 %u", (unsigned)tl);

    /* 超上限：513 → FAKE（帧头 + 长度域即可判定，不必等整帧） */
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 513, nullptr);
    cdstd_feed(s_frame, 16);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "DATA 域 513 超上限应 FAKE");

    /* 长度域 0xFFFF → FAKE（ASan 下不得越界读） */
    fixture_reset();
    n = cdstd_build(s_frame, 0xA0, 0, nullptr);
    app_cdstd_frame_t *f = (app_cdstd_frame_t *)s_frame;
    f->len[0] = 0xFF;
    f->len[1] = 0xFF;
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "长度域 0xFFFF 应 FAKE");
}

/** @brief CRC 不过 → FAKE（不是 SKIP）；坏帧逐字节重跳后真帧仍能找回 */
static void case_crc_fail_is_fake(void)
{
    TEST_BEGIN("CRC 不过 → FAKE（不是 SKIP）：坏帧逐字节重跳，后面的真帧仍能找回");

    /* 一条"声明 512 字节、CRC 被破坏"的坏帧，内部 100 字节处嵌一条真帧。
       **反向验证**：把 CRC 失败改回 SKIP —— 框架会按伪长度（523）整段跳过，
       嵌入的真帧被连坐吞掉，下面的重跳循环找不到它。 */
    static uint8_t bad_payload[512];
    memset(bad_payload, 0x11, sizeof(bad_payload));

    uint16_t n_bad  = cdstd_build(s_frame, 0xA2, 512, bad_payload);
    uint8_t  good[64];
    uint16_t n_good = cdstd_build(good, 0xA5, 20, nullptr);
    CHECK_MSG(n_bad == 523 && n_good == 31, "构造帧长不对（%u / %u）", (unsigned)n_bad,
              (unsigned)n_good);

    memcpy(s_frame + 100, good, n_good);
    s_frame[n_bad - 1] ^= 0x01; /* 破坏 CRC 尾 */

    fixture_reset();
    cdstd_feed(s_frame, n_bad);

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "CRC 不过必须 FAKE —— 返回 SKIP 会按伪长度吞掉后面的字节");

    /* 模拟框架对 FAKE 的处置：跳 1 字节重探，直到认出嵌入的真帧 */
    bool found = false;
    for (int guard = 0; guard < 200; guard++) {
        app_pcb_probe_state_t st = cdstd_probe(&tl, &ax, sizeof(s_scratch_buf));
        if (st == APP_PCB_PROBE_STATE_READY) {
            found = true;
            break;
        }
        if (st != APP_PCB_PROBE_STATE_FAKE) break; /* WAIT / SKIP 都不该出现 */
        rb_skip(&s_ut_rb, 1, nullptr);
    }
    CHECK_MSG(found, "坏帧之后的重跳没能找回嵌入的真帧");
    CHECK_MSG(tl == n_good, "找回的应是嵌入真帧（%u），得到 %u", (unsigned)n_good, (unsigned)tl);
    CHECK_MSG(ax == (0xA5 & 0x0F), "aux 应为 %02X，得到 %02X", 0xA5 & 0x0F, ax);
}

/** @brief 暂存区契约：装不下 → SKIP 并报出整帧长度 */
static void case_scratch_contract(void)
{
    TEST_BEGIN("暂存区契约：装不下 → SKIP 且报出整帧长度");

    uint32_t tl = 0;
    uint8_t  ax = 0;
    uint16_t n  = cdstd_build(s_frame, 0xA4, 512, nullptr);

    fixture_reset();
    cdstd_feed(s_frame, n);
    CHECK_MSG(cdstd_probe(&tl, &ax, 16) == APP_PCB_PROBE_STATE_SKIP,
              "暂存区装不下应 SKIP（不是 READY，也不得越界写）");
    CHECK_MSG(tl == n, "SKIP 也要报出整帧长度（%u），得到 %u", (unsigned)n, (unsigned)tl);

    tl = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, n) == APP_PCB_PROBE_STATE_READY, "恰好装下应 READY");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/** @brief 定容契约：单次可写必须覆盖通道单段上限（RS485 一段可达 2048） */
static void case_rb_capacity_contract(void)
{
    TEST_BEGIN("定容契约：单次可写 ≥ 通道单段上限（RS485 一段可达 2048）");

    /* app_ccb_dispatch 把「传输层一整段」一次 rb_write 进来：装不下先 flush、再被
       rb_write 静默截短 —— 所以容量必须 ≥ 单次最大写入 + 1（环形缓冲留一个空槽
       区分满/空），否则 485 上多帧连发的一段会被从中间切掉。
       2048 = RS485_BUF_SIZE（app_rs485.c）：空闲交付一圈 DMA 的增量上限。
       **反向验证**：把 s_cdstd_rb 改小（如 1024），本用例两条断言立刻红。 */
    const uint32_t single_write_max = 2048U;
    const uint32_t frame_max = (uint32_t)sizeof(app_cdstd_frame_t) + CDSTD_PAYLOAD_MAX + 2U;

    CHECK_MSG((uint32_t)s_cdstd_rb.size >= single_write_max + 1U,
              "RB 容量 %u，装不下通道单段 %u 字节（需 ≥ %u）", (unsigned)s_cdstd_rb.size,
              (unsigned)single_write_max, (unsigned)(single_write_max + 1U));
    CHECK_MSG((uint32_t)s_cdstd_rb.size >= 2U * frame_max + 1U,
              "RB 容量 %u，不足 2 × 最长帧 %u + 1", (unsigned)s_cdstd_rb.size, (unsigned)frame_max);
}

/** @brief 伪帧不得把整帧拷进暂存区（逐字节重跳退化成 O(n²) 的守门） */
static void case_fake_must_not_copy_whole(void)
{
    TEST_BEGIN("伪帧不得拷整帧（框架逐字节重跳，否则退化成 O(n²) 的 memcpy）");

    static uint8_t junk[1400];
    memset(junk, 0x43, sizeof(junk)); /* 一个 0xFF 都不给 */

    fixture_reset();
    cdstd_feed(junk, sizeof(junk));
    memset(s_scratch_buf, 0xEE, sizeof(s_scratch_buf));

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
              "杂物应判 FAKE");

    /* 帧头/长度域最多占前 8 个字节，检查起点取 16 字节的宽松上界：
       起点以后必须原封不动（伪帧逐字节重跳，每次多拷一字节就是 O(n²)）。 */
    bool untouched = true;
    for (uint16_t i = 16; i < sizeof(s_scratch_buf); i++) {
        if (s_scratch_buf[i] != 0xEE) {
            untouched = false;
            break;
        }
    }
    CHECK_MSG(untouched, "FAKE 却把整帧拷进了暂存区 —— 逐字节重跳会退化成 O(n²)");
}

/** @brief 互不毒化：其它协议（IAP/CASC/RLS）的字节流不得被判成 CDSTD 帧 */
static void case_no_cross_poison(void)
{
    TEST_BEGIN("互不毒化：其它协议（IAP/CASC/RLS）的字节流不得判 CDSTD 帧");

    /* 注：LDI 与 CDSTD 的帧几何完全同构（STX、4 字节大端长度、CRC 覆盖 [2,6+L)、
       CRC 落位与字节序全一致），帧层面不可区分；两者不会同时启用（产品约束），
       故不做互不毒化断言。若将来需要共存，先在协议层引入区分手段（如命令域检查）。 */

    /* 其它协议的帧头都过不了 CDSTD 的帧头检查 → FAKE */
    static const struct {
        const char *name;
        uint8_t     head[4];
    } others[] = {
        {"IAP (5A5A5A5A)", {0x5A, 0x5A, 0x5A, 0x5A}},
        {"CASC (A55A)", {0xA5, 0x5A, 0x00, 0x00}},
        {"RLS (FFFE)", {0xFF, 0xFE, 0x00, 0x00}},
    };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        fixture_reset();
        uint8_t buf[64];
        memset(buf, 0x11, sizeof(buf));
        memcpy(buf, others[i].head, 4);
        cdstd_feed(buf, sizeof(buf));

        uint32_t tl2 = 0;
        uint8_t  ax2 = 0;
        CHECK_MSG(cdstd_probe(&tl2, &ax2, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_FAKE,
                  "%s 的字节流被判成 CDSTD 帧", others[i].name);
    }
}

/** @brief 一条帧分两段写入、且第二段让 RB 回绕 → 探针仍要能解析 */
static void case_split_across_rb_wrap(void)
{
    TEST_BEGIN("一条帧分两段写入、且第二段让 RB 回绕 → 仍要能解析");

    fixture_reset();
    uint16_t n = cdstd_build(s_frame, 0xA7, 300, nullptr);

    /* 先把读写指针推到"接近缓冲末尾"：这样第二段写必然让写指针绕回去。
       推进量按缓冲容量算，不写死 —— 换容量也不会越界。 */
    const uint16_t split   = (uint16_t)(n / 2);
    const uint16_t advance = (uint16_t)(s_ut_rb.size - 1U - split);

    static uint8_t junk[4096];
    static uint8_t sink[4096];
    memset(junk, 0xA5, sizeof(junk));
    cdstd_feed(junk, advance);
    CHECK_MSG(rb_read(&s_ut_rb, sink, advance, nullptr) == advance, "预置：把指针推到接近末尾");
    CHECK_MSG(rb_avail(&s_ut_rb, nullptr) == 0, "预置：缓冲应为空");

    const uint16_t w1 = rb_write(&s_ut_rb, s_frame, split, nullptr);
    const uint16_t w2 = rb_write(&s_ut_rb, s_frame + split, (uint16_t)(n - split), nullptr);
    CHECK_MSG(w1 == split && w2 == (uint16_t)(n - split), "两段都要完整写进去（%u + %u）",
              (unsigned)w1, (unsigned)w2);
    CHECK_MSG(s_ut_rb.write_index < s_ut_rb.read_index,
              "本用例要求第二段真的让写指针绕回（read=%u write=%u）—— 没绕回就测不到东西",
              (unsigned)s_ut_rb.read_index, (unsigned)s_ut_rb.write_index);

    uint32_t tl = 0;
    uint8_t  ax = 0;
    CHECK_MSG(cdstd_probe(&tl, &ax, sizeof(s_scratch_buf)) == APP_PCB_PROBE_STATE_READY,
              "分两段写入 + RB 内部回绕之后探针认不出这条帧");
    CHECK_MSG(tl == n, "total_len 应为 %u，得到 %u", (unsigned)n, (unsigned)tl);
}

/* ================================================================
 *  任务侧：取帧与分派
 * ================================================================ */

static int calls_total(void)
{
    int n = 0;
    for (int i = 0; i < CDSTD_CMD_COUNT; i++) n += s_calls[i];
    return n;
}

static bool wait_calls(int idx, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (s_calls[idx] >= want) return true;
        usleep(5000);
    }
    return s_calls[idx] >= want;
}

static void case_task_dispatch(void)
{
    TEST_BEGIN("任务：空队列不误取、真帧按 aux 分派、越界命令丢弃");

    _cdstd_module_init();
    CHECK_MSG(s_bind_calls == 2, "应绑定 udp/rs485 两个通道（得到 %d）", s_bind_calls);
    CHECK_MSG(s_cdstd_msg_queue != nullptr, "帧队列应已建立");

    /* 空队列：任务反复超时也不能拿未初始化的 msg 去查表
       **反向验证**：把取帧判断反过来（osOK == 时 continue），超时分支会拿零初始化
       的 msg 去查表，下面这条"不得有分派"立刻红。 */
    usleep(400 * 1000);
    CHECK_MSG(calls_total() == 0, "空队列时不应有任何分派（得到 %d 次）", calls_total());

    /* 投一帧：aux=3，帧内长度域=100 */
    static uint8_t msg_buf[CDSTD_MSG_SIZE] __attribute__((aligned(4)));
    app_dispatch_msg_t *m = (app_dispatch_msg_t *)msg_buf;
    static app_ccb_t s_chan = {.name = "cdstd_ut_chan"};

    uint8_t  frame[128];
    uint16_t n = cdstd_build(frame, 0xA3, 100, nullptr);

    m->ccb = &s_chan;
    m->aux = 3;
    memcpy(m->data, frame, n);
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    CHECK_MSG(wait_calls(3, 1, 2000), "投递的帧未被分派（取帧判断或表查询仍有问题）");
    CHECK_MSG(s_last_len[3] == 100, "处理函数收到的 data_len 应为帧内长度域 100，得到 %u",
              (unsigned)s_last_len[3]);
    CHECK_MSG(s_last_ccb[3] == &s_chan, "处理函数应收到投递时的 ccb");

    /* 越界 aux（>= CDSTD_CMD_COUNT）：直接丢，不查表 */
    m->aux = CDSTD_CMD_COUNT;
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    usleep(200 * 1000);
    CHECK_MSG(calls_total() == 1, "越界命令不应产生分派（总数 %d）", calls_total());

    /* 其后的一条真帧不受影响 */
    m->aux = 5;
    CHECK_MSG(osMessageQueuePut(s_cdstd_msg_queue, m, 0, 0) == osOK, "投递失败");
    CHECK_MSG(wait_calls(5, 1, 2000), "越界命令之后真帧未被分派");
    CHECK_MSG(s_last_len[5] == 100, "处理函数收到的 data_len 应为 100，得到 %u",
              (unsigned)s_last_len[5]);
    CHECK_MSG(calls_total() == 2, "总派发次数应为 2，得到 %d", calls_total());
}

/* ================================================================ */

int main(void)
{
    printf("CDSTD 协议 host 单测\n");

    case_probe_states();
    case_wait_whole_frame();
    case_length_domain();
    case_crc_fail_is_fake();
    case_scratch_contract();
    case_rb_capacity_contract();
    case_fake_must_not_copy_whole();
    case_no_cross_poison();
    case_split_across_rb_wrap();
    case_task_dispatch();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
