/**
 * @file    test_probes.c
 * @brief   各协议探针的 host 单测（IAP / LDI / RLS）
 *
 * 探针是纯函数：给定环形缓冲区、暂存区与来源通道，返回四态之一。这里直接调用
 * 真实探针（app_iap.c / app_ldi.c / app_rls.c 原样编译），只自建一个最小 pcb
 * 与 ring buffer，不启动任何任务、不建队列。
 *
 * 重点覆盖"缓冲区容量"与"长度域校验"这两类：此前修掉的探针缺陷全部属于这一类
 * （把 avail 当长度写进固定大小的暂存区、未校验长度域就索引），
 * 开 ASan 后这些越界会被直接抓住。
 *
 * 帧构造用与探针相同的 CRC 函数，因此校验路径必然通过；这样测的是探针的
 * **结构判定与边界处理**，而不是 CRC 算法本身是否与硬件一致。
 *
 * 来源：从参考工程 Project_STD_B/test/test_probes.c 移植。本工程没有 AH_MQTT 的
 * pcb（该模块改造中），故参考工程里对应的部分不存在，这里也没有。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app_dispatch.h"
#include "app_iap.h"
#include "app_ldi.h"
#include "app_ldi_cmd.h"
#include "crc_utils.h"
#include "pl_crc.h"

#include "app_rls.h"

/* ================================================================
 *  断言与统计
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  装置
 * ================================================================ */

#define RB_SIZE (2048U)

static uint8_t s_rb_buf[RB_SIZE];
static ring_buffer_t s_rb = {.data = s_rb_buf, .size = RB_SIZE, .mutex = NULL};

static pcb_t s_pcb = {.name = "probe_ut", .rb = &s_rb};
static ccb_t s_ccb = {.name = "chan", .ops = NULL};

static uint8_t s_scratch[FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));

static uint8_t s_frame[1200] __attribute__((aligned(4)));

/** @brief 清空缓冲区并放入一段字节 */
static void feed(const void *data, size_t len)
{
    rb_flush(&s_rb, NULL);
    rb_write(&s_rb, (const uint8_t *)data, (uint16_t)len, NULL);
}

/* ---- IAP ---- */

static uint32_t s_iap_len;
static uint8_t s_iap_aux;

static pcb_probe_sta_t iap_probe(void)
{
    return iap_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &s_iap_len,
                           &s_iap_aux);
}

/**
 * @brief 构造 IAP 帧
 * @param len_words DATA 域字数（0 ~ 256）
 * @return 整帧字节数
 */
static size_t iap_build(uint32_t cmd, uint32_t len_words)
{
    iap_frame_t *f = (iap_frame_t *)s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    f->head = FRAME_HEAD;
    f->seq  = 0x11223344U;
    f->cmd  = cmd;
    f->len  = len_words;
    for (uint32_t i = 0; i < len_words; i++)
        f->data_crc[i] = 0xA0000000U + i;

    /* CRC 覆盖帧头 + DATA（不含 CRC 字本身），与探针的调用完全一致 */
    f->data_crc[len_words] =
        pl_crc32_calc(pl_crc_get_handle(), s_frame, sizeof(iap_frame_t) + len_words * 4);
    return (size_t)sizeof(iap_frame_t) + len_words * 4 + 4;
}

/* ---- LDI ---- */

static uint32_t s_ldi_len;
static uint8_t s_ldi_aux;

static pcb_probe_sta_t ldi_probe(void)
{
    return ldi_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &s_ldi_len,
                           &s_ldi_aux);
}

/** @brief LDI DATA 域上限（须与 app_ldi.c 的 LDI_DATA_MAX 一致；两侧边界都会测）*/
#define TEST_LDI_DATA_MAX (512U)

static size_t ldi_build(uint8_t cmd, uint32_t data_len)
{
    ldi_frame_t *f = (ldi_frame_t *)s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    f->stx[0] = 0xFF;
    f->stx[1] = 0xFF;
    f->ver    = 0x00;
    f->seq    = 0x21;
    f->len[0] = (uint8_t)(data_len >> 24);
    f->len[1] = (uint8_t)(data_len >> 16);
    f->len[2] = (uint8_t)(data_len >> 8);
    f->len[3] = (uint8_t)data_len;

    f->data_crc[0] = cmd;
    for (uint32_t i = 1; i < data_len; i++)
        f->data_crc[i] = (uint8_t)(0x30 + i);

    /* CRC 覆盖 VER ~ DATA 尾，与探针的 crc16_xmodem(&frame->ver, data_len + 6) 一致 */
    uint16_t crc = crc16_xmodem(&f->ver, data_len + sizeof(ldi_frame_t) - sizeof(f->stx));
    f->data_crc[data_len]     = (uint8_t)(crc >> 8);
    f->data_crc[data_len + 1] = (uint8_t)crc;
    return (size_t)sizeof(ldi_frame_t) + data_len + 2;
}

/* ---- RLS ---- */

static uint32_t s_rls_len;
static uint8_t s_rls_aux;

static pcb_probe_sta_t rls_probe(void)
{
    return rls_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &s_rls_len,
                           &s_rls_aux);
}

/** @brief RLS 整帧上限（须与 app_rls.c 的 RLS_PAYLOAD_MAX 一致）*/
#define TEST_RLS_FRAME_MAX (530U)

static size_t rls_build(uint16_t total_len)
{
    rls_frame_t *f = (rls_frame_t *)s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    f->head[0]   = 0xFF;
    f->head[1]   = 0xFE;
    f->length[0] = (uint8_t)(total_len >> 8); /* 大端：整个帧长 */
    f->length[1] = (uint8_t)total_len;
    f->cmd[0]    = 0x4D;
    f->cmd[1]    = 0x42;

    /* 帧头之后、帧尾之前填数据（顺序无关，探针不校验 BCC）*/
    for (size_t i = 0; i + 2 < (size_t)total_len - sizeof(rls_frame_t); i++)
        f->data_bcc_tail[i] = (uint8_t)i;

    f->data_bcc_tail[total_len - 2 - sizeof(rls_frame_t)] = 0x0D; /* 帧尾 */
    f->data_bcc_tail[total_len - 1 - sizeof(rls_frame_t)] = 0x0C;
    return total_len;
}

/* ================================================================
 *  IAP 探针
 * ================================================================ */

static void test_iap(void)
{
    TEST_BEGIN("IAP 探针");

    /* 合法帧 */
    size_t n = iap_build(0x05, 4);
    feed(s_frame, n);
    CHECK(iap_probe() == PCB_PROBE_READY);
    CHECK(s_iap_len == n);
    CHECK(s_iap_aux == 0x05);

    /* 数据不足 → WAIT */
    feed(s_frame, 8);
    CHECK(iap_probe() == PCB_PROBE_WAIT);

    /* 帧头错 → FAKE */
    iap_build(0x05, 4);
    s_frame[0] = 0x00;
    feed(s_frame, n);
    CHECK(iap_probe() == PCB_PROBE_FAKE);

    /* payload_len 超协议上限（256 字）→ FAKE */
    iap_build(0x05, 257);
    feed(s_frame, sizeof(iap_frame_t) + 257 * 4 + 4);
    CHECK(iap_probe() == PCB_PROBE_FAKE);

    /* 边界：恰好 256 字是合法的 */
    iap_build(0x05, 256);
    feed(s_frame, sizeof(iap_frame_t) + 256 * 4 + 4);
    CHECK(iap_probe() == PCB_PROBE_READY);

    /* CRC 错 → FAKE */
    n = iap_build(0x05, 4);
    s_frame[n - 1] ^= 0xFF;
    feed(s_frame, n);
    CHECK(iap_probe() == PCB_PROBE_FAKE);

    /* 数据不足但后面又出现一个新帧头 → 提前判定 FAKE（不等数据到齐）*/
    n = iap_build(0x05, 8); /* 需要 52 字节，只喂 40 */
    feed(s_frame, 40);
    CHECK(iap_probe() == PCB_PROBE_WAIT);

    iap_build(0x05, 8);
    memcpy(s_frame + 20, &(uint32_t){FRAME_HEAD}, 4); /* 半路插入帧头 */
    feed(s_frame, 40);
    CHECK(iap_probe() == PCB_PROBE_FAKE);
}

/* ================================================================
 *  LDI 探针
 * ================================================================ */

static void test_ldi(void)
{
    TEST_BEGIN("LDI 探针");

    g_ldi.cfg_valid = true; /* 让身份校验分支可达 */

    /* 合法帧（配置指令 0AH 不校验身份）*/
    size_t n = ldi_build(LDI_CMD_SET_IP_REQ, 20);
    feed(s_frame, n);
    CHECK(ldi_probe() == PCB_PROBE_READY);
    CHECK(s_ldi_len == n);
    CHECK(s_ldi_aux == LDI_CMD_SET_IP_REQ);

    /* 数据不足 → WAIT */
    feed(s_frame, 10);
    CHECK(ldi_probe() == PCB_PROBE_WAIT);

    /* 帧头错 → FAKE */
    ldi_build(LDI_CMD_SET_IP_REQ, 20);
    s_frame[1] = 0x00;
    feed(s_frame, n);
    CHECK(ldi_probe() == PCB_PROBE_FAKE);

    /* 版本号错 → FAKE */
    ldi_build(LDI_CMD_SET_IP_REQ, 20);
    s_frame[2] = 0x01;
    feed(s_frame, n);
    CHECK(ldi_probe() == PCB_PROBE_FAKE);

    /* 长度域超上限 → FAKE（此前未校验时会用 data_len 索引，越界读）*/
    ldi_build(LDI_CMD_SET_IP_REQ, TEST_LDI_DATA_MAX + 1);
    feed(s_frame, sizeof(ldi_frame_t) + TEST_LDI_DATA_MAX + 1 + 2);
    CHECK(ldi_probe() == PCB_PROBE_FAKE);

    /* 长度域为极大值（0xFFFFFFFF）→ FAKE，且不得越界 */
    ldi_build(LDI_CMD_SET_IP_REQ, 20);
    s_frame[4] = 0xFF;
    s_frame[5] = 0xFF;
    s_frame[6] = 0xFF;
    s_frame[7] = 0xFF;
    feed(s_frame, 64);
    CHECK(ldi_probe() == PCB_PROBE_FAKE);

    /* 边界：恰好上限且数据齐全 → READY */
    n = ldi_build(LDI_CMD_SET_IP_REQ, TEST_LDI_DATA_MAX);
    feed(s_frame, n);
    CHECK_MSG(ldi_probe() == PCB_PROBE_READY, "DATA 域上限边界应被接受（%u 字节）",
              (unsigned)TEST_LDI_DATA_MAX);

    /* CRC 错 → FAKE */
    n = ldi_build(LDI_CMD_SET_IP_REQ, 20);
    s_frame[n - 1] ^= 0xFF;
    feed(s_frame, n);
    CHECK(ldi_probe() == PCB_PROBE_FAKE);

    /* 身份不匹配 → SKIP（帧合法但不是本机）*/
    memcpy(g_ldi.cfg.lane_hex, "99999", 5);
    memcpy(g_ldi.cfg.cert, "AAAAAAAA", 8);
    n = ldi_build(LDI_CMD_CTRL_REQ, 24); /* 非配置指令 → 走身份校验 */
    feed(s_frame, n);
    CHECK(ldi_probe() == PCB_PROBE_SKIP);
    CHECK(s_ldi_len == n);

    /* 身份匹配 → READY（1BH 用 ldi_ctrl_head_t：lane_code 偏移 9、cert_info 偏移 14）*/
    memcpy(g_ldi.cfg.lane_hex, s_frame + 8 + 9, 5);
    memcpy(g_ldi.cfg.cert, s_frame + 8 + 14, 8);
    feed(s_frame, n);
    CHECK_MSG(ldi_probe() == PCB_PROBE_READY, "身份匹配的帧应被接受");
}

/* ================================================================
 *  RLS 探针
 * ================================================================ */

static void test_rls(void)
{
    TEST_BEGIN("RLS 探针");

    /* 合法帧 */
    size_t n = rls_build(64);
    feed(s_frame, n);
    CHECK(rls_probe() == PCB_PROBE_READY);
    CHECK(s_rls_len == 64);

    /* 数据不足 → WAIT */
    feed(s_frame, 8); /* 小于 sizeof(rls_frame_t) + 4 */
    CHECK(rls_probe() == PCB_PROBE_WAIT);

    /* 帧头错 → FAKE */
    rls_build(64);
    s_frame[0] = 0x00;
    feed(s_frame, n);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 帧尾错 → FAKE */
    rls_build(64);
    s_frame[62] = 0x00;
    feed(s_frame, n);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 长度域超上限 → FAKE（此前会按 data_len 向后索引，越界读）*/
    rls_build(64);
    s_frame[2] = 0x02; /* 长度改成 0x02xx */
    s_frame[3] = 0x20;
    feed(s_frame, 64);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 长度域 = 0 → FAKE（此前 data_len-2 向前越界，且会让框架零进度空转）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x00;
    feed(s_frame, 64);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 长度域 = 1 → FAKE（下边界，data_len-2 仍会向前越界）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x01;
    feed(s_frame, 64);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 长度域 = 帧长下限 -1 → FAKE（RLS_FRAME_MIN = sizeof(rls_frame_t) + 3 = 9）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x08;
    feed(s_frame, 64);
    CHECK(rls_probe() == PCB_PROBE_FAKE);

    /* 边界：恰好上限 → READY */
    n = rls_build(TEST_RLS_FRAME_MAX);
    feed(s_frame, n);
    CHECK_MSG(rls_probe() == PCB_PROBE_READY, "上限边界应被接受（%u 字节）",
              (unsigned)TEST_RLS_FRAME_MAX);

    /* 上限 +1 → FAKE */
    n = rls_build(TEST_RLS_FRAME_MAX + 1);
    feed(s_frame, n);
    CHECK(rls_probe() == PCB_PROBE_FAKE);
}

/* ================================================================
 *  暂存区容量契约
 * ================================================================ */

static void test_scratch_contract(void)
{
    TEST_BEGIN("暂存区容量契约：装不下时返回 SKIP 且不越界");

    uint32_t len = 0;
    uint8_t aux  = 0;

    /* IAP：给一个肯定装不下的暂存区 */
    size_t n = iap_build(0x05, 8);
    feed(s_frame, n);
    pcb_probe_sta_t st =
        iap_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, 16, &len, &aux); /* 16 < 帧长 */
    CHECK(st == PCB_PROBE_SKIP);
    CHECK(len == n);

    /* LDI 同理 */
    n = ldi_build(LDI_CMD_SET_IP_REQ, 20);
    feed(s_frame, n);
    st = ldi_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, 16, &len, &aux);
    CHECK(st == PCB_PROBE_SKIP);
    CHECK(len == n);

    /* RLS 同理 */
    n = rls_build(64);
    feed(s_frame, n);
    st = rls_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch, 16, &len, &aux);
    CHECK(st == PCB_PROBE_SKIP);
    CHECK(len == n);
}

/* ================================================================
 *  通用探针契约 —— 不变量测试
 *
 *  上面三个 test_xxx 覆盖的是"我知道会错的地方"；这一节覆盖"我没想到的地方"：
 *  把三个探针放进同一张表，用随机与构造字节流反复调用，只断言**不变量**。
 *  新增协议只要加一行表项就自动受检，不依赖"记得写用例"。
 *
 *  这四条不变量正是此前那批探针缺陷的形状：
 *    · READY/SKIP 的帧长必须为正     —— 否则框架零进度空转
 *    · READY 的帧长 ≤ payload_max   —— 否则框架会读穿暂存区
 *    · READY 的帧长 ≤ 当前可用字节   —— 否则是在声明不存在的数据
 *    · WAIT 不得消费                 —— 否则等更多数据时会丢帧
 *  （越界读写由 ASan 直接抓，不必写成断言）
 * ================================================================ */

typedef struct {
    const char       *name;
    pcb_probe_fn_t  probe;
    uint16_t          payload_max;
    size_t          (*build)(uint32_t variant); /**< 在 s_frame 里造一帧，返回长度 */
} probe_entry_t;

static size_t build_iap(uint32_t v)
{
    return iap_build(0x05, v % 17);
}
static size_t build_ldi(uint32_t v)
{
    return ldi_build(LDI_CMD_SET_IP_REQ, 20 + (v % 64));
}
static size_t build_rls(uint32_t v)
{
    return rls_build((uint16_t)(16 + (v % 120)));
}

static const probe_entry_t s_probes[] = {
    {"iap", iap_probe_frame, 1044, build_iap},
    {"ldi", ldi_probe_frame, 522, build_ldi},
    {"rls", rls_probe_frame, 530, build_rls},
};

/* 确定性伪随机（xorshift32）：失败可复现，不依赖平台 rand 的实现 */
static uint32_t s_rng = 0x12345678U;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/** @brief 在当前缓冲区内容上反复调用探针，断言四条不变量 */
static void conformance_run(const probe_entry_t *pe)
{
    for (int step = 0; step < 512; step++) {
        uint16_t avail = rb_avail(&s_rb, NULL);
        if (avail == 0) return;

        uint32_t total = 0;
        uint8_t aux    = 0;
        pcb_probe_sta_t st =
            pe->probe(&s_pcb, &s_ccb, nullptr, s_scratch, sizeof(s_scratch), &total, &aux);

        if (st == PCB_PROBE_WAIT) {
            CHECK_MSG(rb_avail(&s_rb, NULL) == avail, "%s: WAIT 消费了数据", pe->name);
            return; /* 等更多数据，此处没有了 */
        }

        if (st == PCB_PROBE_READY || st == PCB_PROBE_SKIP) {
            CHECK_MSG(total > 0, "%s: %s 返回帧长 0（会让框架零进度空转）", pe->name,
                      st == PCB_PROBE_READY ? "READY" : "SKIP");
            CHECK_MSG(total <= avail, "%s: %s 声明帧长 %u 超过可用字节 %u", pe->name,
                      st == PCB_PROBE_READY ? "READY" : "SKIP", total, avail);
            if (st == PCB_PROBE_READY)
                CHECK_MSG(total <= pe->payload_max, "%s: READY 帧长 %u 超过 payload_max %u",
                          pe->name, total, pe->payload_max);
        }

        uint16_t skip = 0;
        if (st == PCB_PROBE_READY || st == PCB_PROBE_SKIP)
            skip = (uint16_t)total;
        else
            skip = 1; /* FAKE */

        if (rb_skip(&s_rb, skip, NULL) == 0) return; /* 数据不足，收敛 */
    }
}

static void test_conformance(void)
{
    TEST_BEGIN("通用契约：随机与构造字节流下的探针不变量");

    for (size_t i = 0; i < sizeof(s_probes) / sizeof(s_probes[0]); i++) {
        const probe_entry_t *pe = &s_probes[i];

        /* 1) 纯随机字节流（长度也随机）—— 最容易撞出越界 */
        for (int t = 0; t < 300; t++) {
            size_t len = rnd() % 1200U;
            for (size_t k = 0; k < len; k++)
                s_frame[k] = (uint8_t)rnd();
            feed(s_frame, len);
            conformance_run(pe);
        }

        /* 2) 合法帧 + 随机破坏若干字节。破坏点偏向前 16 字节 —— 三个协议的长度域
              都在那里，于是长度校验分支被均匀覆盖，而不必为每个协议写构造代码 */
        for (int t = 0; t < 300; t++) {
            size_t len = pe->build(rnd());
            int corruptions = 1 + (int)(rnd() % 4);
            for (int c = 0; c < corruptions; c++) {
                size_t pos = (rnd() % 3 == 0) ? (rnd() % len) : (rnd() % 16);
                if (pos < len) s_frame[pos] ^= (uint8_t)(1U << (rnd() % 8));
            }
            feed(s_frame, len);
            conformance_run(pe);
        }
    }
}

/* ================================================================
 *  入口
 * ================================================================ */

int main(void)
{
    printf("协议探针 host 单测（IAP / LDI / RLS）\n");

    test_iap();
    test_ldi();
    test_rls();
    test_scratch_contract();
    test_conformance();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
