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
 * 来源：从参考工程 Project_STD_B/test/test_probes.c 移植。本工程没有 AHMQ 的
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

static app_pcb_t s_pcb = {.name = "probe_ut", .rb = &s_rb};
static app_ccb_t s_ccb = {.name = "chan", .ops = NULL};

__attribute__((aligned(4))) static uint8_t s_scratch_buf[FRAME_DATA_MAX_LEN];

__attribute__((aligned(4))) static uint8_t s_frame[1200];

/** @brief 清空缓冲区并放入一段字节 */
static void feed(const void *data, size_t len)
{
    rb_flush(&s_rb, NULL);
    rb_write(&s_rb, (const uint8_t *)data, (uint16_t)len, NULL);
}

/* ---- IAP ---- */

static uint32_t s_iap_len;
static uint8_t s_iap_aux;

static app_pcb_probe_state_t iap_probe(void)
{
    return app_iap_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), &s_iap_len,
                           &s_iap_aux);
}

/**
 * @brief 构造 IAP 帧
 * @param cmd       命令码
 * @param len_words DATA 域字数（0 ~ 256）
 * @return 整帧字节数
 */
static size_t iap_build(uint32_t cmd, uint32_t len_words)
{
    app_iap_frame_t *f = (app_iap_frame_t *)s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    f->head = FRAME_HEAD;
    f->seq  = 0x11223344U;
    f->cmd  = cmd;
    f->len  = len_words;
    for (uint32_t i = 0; i < len_words; i++)
        f->data_crc[i] = 0xA0000000U + i;

    /* CRC 覆盖帧头 + DATA（不含 CRC 字本身），与探针的调用完全一致 */
    f->data_crc[len_words] =
        pl_crc32_calc(pl_crc_get_handle(), s_frame, sizeof(app_iap_frame_t) + len_words * 4);
    return (size_t)sizeof(app_iap_frame_t) + len_words * 4 + 4;
}

/* ---- LDI ---- */

static uint32_t s_ldi_len;
static uint8_t s_ldi_aux;

static app_pcb_probe_state_t ldi_probe(void)
{
    return app_ldi_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), &s_ldi_len,
                           &s_ldi_aux);
}

/** @brief LDI DATA 域上限（须与 app_ldi.c 的 LDI_DATA_MAX 一致；两侧边界都会测）*/
#define TEST_LDI_DATA_MAX (512U)

static size_t ldi_build(uint8_t cmd, uint32_t data_len)
{
    app_ldi_frame_t *f = (app_ldi_frame_t *)s_frame;
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
    uint16_t crc = crc16_xmodem(&f->ver, data_len + sizeof(app_ldi_frame_t) - sizeof(f->stx));
    f->data_crc[data_len]     = (uint8_t)(crc >> 8);
    f->data_crc[data_len + 1] = (uint8_t)crc;
    return (size_t)sizeof(app_ldi_frame_t) + data_len + 2;
}

/* ---- RLS ---- */

static uint32_t s_rls_len;
static uint8_t s_rls_aux;

static app_pcb_probe_state_t rls_probe(void)
{
    return app_rls_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), &s_rls_len,
                           &s_rls_aux);
}

/** @brief RLS 整帧上限（须与 app_rls.c 的 RLS_PAYLOAD_MAX 一致）*/
#define TEST_RLS_FRAME_MAX (530U)

static size_t rls_build(uint16_t total_len)
{
    app_rls_frame_t *f = (app_rls_frame_t *)s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    f->head[0]   = 0xFF;
    f->head[1]   = 0xFE;
    f->length[0] = (uint8_t)(total_len >> 8); /* 大端：整个帧长 */
    f->length[1] = (uint8_t)total_len;
    f->cmd[0]    = 0x4D;
    f->cmd[1]    = 0x42;

    /* 帧头之后、帧尾之前填数据（顺序无关，探针不校验 BCC）*/
    for (size_t i = 0; i + 2 < (size_t)total_len - sizeof(app_rls_frame_t); i++)
        f->data_bcc_tail[i] = (uint8_t)i;

    f->data_bcc_tail[total_len - 2 - sizeof(app_rls_frame_t)] = 0x0D; /* 帧尾 */
    f->data_bcc_tail[total_len - 1 - sizeof(app_rls_frame_t)] = 0x0C;
    return total_len;
}

/* ================================================================
 *  IAP 探针
 * ================================================================ */

/** @brief IAP 探针的四态与长度上下界：合法帧 READY、不足 WAIT、帧头/CRC/超限 FAKE
 *
 *  长度域是帧内取来的 32 位值，**两侧都要夹**：上界 256 word（超限即伪帧，否则后面
 *  的 CRC 读会按它索引到暂存区之外）；下界 0（最小合法帧 = 5 word，CRC 是唯一载荷字）
 *  也要真的能通过 —— 只测上界会把"合法最小帧被误杀"漏掉。 */
static void test_iap(void)
{
    TEST_BEGIN("IAP 探针");

    /* 合法帧 */
    size_t n = iap_build(0x05, 4);
    feed(s_frame, n);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_READY);
    CHECK(s_iap_len == n);
    CHECK(s_iap_aux == 0x05);

    /* 数据不足 → WAIT */
    feed(s_frame, 8);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_WAIT);

    /* 帧头错 → FAKE */
    iap_build(0x05, 4);
    s_frame[0] = 0x00;
    feed(s_frame, n);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* payload_len 超协议上限（256 字）→ FAKE */
    iap_build(0x05, 257);
    feed(s_frame, sizeof(app_iap_frame_t) + 257 * 4 + 4);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 边界：恰好 256 字是合法的 */
    iap_build(0x05, 256);
    feed(s_frame, sizeof(app_iap_frame_t) + 256 * 4 + 4);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_READY);

    /* 下界：DATA 域 0 字（帧长即 FRAME_MIN_LEN 5 word = 20 字节）是合法最小帧，不是伪帧。
       CRC 覆盖 4 个头字，探针按 `(len + 4) * 4` 校验 —— 长度 0 时正好只算头字。
       （反向验证：把校验长度改成 len*4，本帧立刻变成 FAKE。） */
    n = iap_build(0x05, 0);
    CHECK_MSG(n == sizeof(app_iap_frame_t) + 4, "最小帧构造应为 20 字节，得到 %u", (unsigned)n);
    feed(s_frame, n);
    CHECK_MSG(iap_probe() == APP_PCB_PROBE_STATE_READY, "0 字载荷的最小帧应 READY（下界 0 合法）");
    CHECK(s_iap_len == n);
    CHECK(s_iap_aux == 0x05);

    /* 最小帧少最后 1 个字节（CRC 字不齐）→ WAIT：不足时绝不能提前放行/判死 */
    feed(s_frame, n - 1);
    CHECK_MSG(iap_probe() == APP_PCB_PROBE_STATE_WAIT, "最小帧缺 1 字节应 WAIT（不足 ≠ 伪帧）");

    /* CRC 错 → FAKE */
    n = iap_build(0x05, 4);
    s_frame[n - 1] ^= 0xFF;
    feed(s_frame, n);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 数据不足但后面又出现一个新帧头 → 提前判定 FAKE（不等数据到齐）*/
    n = iap_build(0x05, 8); /* 需要 52 字节，只喂 40 */
    feed(s_frame, 40);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_WAIT);

    iap_build(0x05, 8);
    memcpy(s_frame + 20, &(uint32_t){FRAME_HEAD}, 4); /* 半路插入帧头 */
    feed(s_frame, 40);
    CHECK(iap_probe() == APP_PCB_PROBE_STATE_FAKE);
}

/* ================================================================
 *  LDI 探针
 * ================================================================ */

/** @brief LDI 探针：帧头/版本/长度域/CRC 四关，配置指令免身份、其余需 lane/cert 匹配
 *
 *  身份不匹配是 SKIP（整帧丢弃）而不是 FAKE：帧结构合法、只是不属于本机，
 *  逐字节重跳会把后面同一条流里的真帧连坐吞掉。长度域两侧（0 与 512）都要能通过。 */
static void test_ldi(void)
{
    TEST_BEGIN("LDI 探针");

    g_ldi_ctx.cfg_valid = true; /* 让身份校验分支可达 */

    /* 合法帧（配置指令 0AH 不校验身份）*/
    size_t n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    feed(s_frame, n);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_READY);
    CHECK(s_ldi_len == n);
    CHECK(s_ldi_aux == APP_LDI_CMD_TYPE_SET_IP_REQ);

    /* 数据不足 → WAIT */
    feed(s_frame, 10);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_WAIT);

    /* 帧头错 → FAKE */
    ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    s_frame[1] = 0x00;
    feed(s_frame, n);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 版本号错 → FAKE */
    ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    s_frame[2] = 0x01;
    feed(s_frame, n);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域超上限 → FAKE（此前未校验时会用 data_len 索引，越界读）*/
    ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, TEST_LDI_DATA_MAX + 1);
    feed(s_frame, sizeof(app_ldi_frame_t) + TEST_LDI_DATA_MAX + 1 + 2);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域为极大值（0xFFFFFFFF）→ FAKE，且不得越界 */
    ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    s_frame[4] = 0xFF;
    s_frame[5] = 0xFF;
    s_frame[6] = 0xFF;
    s_frame[7] = 0xFF;
    feed(s_frame, 64);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 边界：恰好上限且数据齐全 → READY */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, TEST_LDI_DATA_MAX);
    feed(s_frame, n);
    CHECK_MSG(ldi_probe() == APP_PCB_PROBE_STATE_READY, "DATA 域上限边界应被接受（%u 字节）",
              (unsigned)TEST_LDI_DATA_MAX);

    /* 下界：DATA 域 = 1 字节（只有命令码、没有请求头）是"命令码可读"的最小帧。
       用配置指令 → 免身份校验 → READY，total = 11。探针入口另有"可见 ≥ 帧头+请求头+2
       = 30 字节"的门禁（身份字段读到 data_crc[21]），故帧后补 20 字节无关数据。 */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 1);
    CHECK_MSG(n == sizeof(app_ldi_frame_t) + 3, "1 长度帧构造应为 11 字节，得到 %u", (unsigned)n);
    feed(s_frame, n);
    static const uint8_t pad[20] = {0};
    rb_write(&s_rb, pad, sizeof(pad), NULL);
    CHECK_MSG(ldi_probe() == APP_PCB_PROBE_STATE_READY, "1 字节 DATA（只有命令码）的帧应 READY");
    CHECK(s_ldi_len == n);

    /* 长度域 = 0：DATA 只剩 CRC 两个字，帧内**没有命令字节**。实测既不 FAKE 也不 WAIT ——
       结构/CRC 全通过，命令码由 data_crc[0]（此刻恰是 CRC 高字节）顶替，再按身份分支
       给 READY/SKIP。这是"长度判据只有上界"的直接后果，不在这里钉死具体归档；
       只钉"不是伪帧"：若哪天给 LDI 加"DATA 至少 1 字节"的下界判据，本用例先红 ——
       改的人必须同时定 0 长度帧的归档（丢弃 / 伪帧），而不是随手加一句 return。 */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 0);
    CHECK_MSG(n == sizeof(app_ldi_frame_t) + 2, "0 长度帧构造应为 10 字节，得到 %u", (unsigned)n);
    feed(s_frame, n);
    CHECK_MSG(ldi_probe() == APP_PCB_PROBE_STATE_WAIT,
              "可见字节不足门禁（10 < 30）时应 WAIT —— 尚不能确认帧不合法");
    rb_write(&s_rb, pad, sizeof(pad), NULL);
    app_pcb_probe_state_t st0 = ldi_probe();
    CHECK_MSG(st0 != APP_PCB_PROBE_STATE_FAKE, "长度域 0 不超上界，不应判伪帧（得到 %u）",
              (unsigned)st0);

    /* CRC 错 → FAKE */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    s_frame[n - 1] ^= 0xFF;
    feed(s_frame, n);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 身份不匹配 → SKIP（帧合法但不是本机）*/
    memcpy(g_ldi_ctx.cfg.lane_hex, "99999", 5);
    memcpy(g_ldi_ctx.cfg.cert, "AAAAAAAA", 8);
    n = ldi_build(APP_LDI_CMD_TYPE_CTRL_REQ, 24); /* 非配置指令 → 走身份校验 */
    feed(s_frame, n);
    CHECK(ldi_probe() == APP_PCB_PROBE_STATE_SKIP);
    CHECK(s_ldi_len == n);

    /* 身份匹配 → READY（1BH 用 app_ldi_ctrl_head_t：lane_code 偏移 9、cert_info 偏移 14）*/
    memcpy(g_ldi_ctx.cfg.lane_hex, s_frame + 8 + 9, 5);
    memcpy(g_ldi_ctx.cfg.cert, s_frame + 8 + 14, 8);
    feed(s_frame, n);
    CHECK_MSG(ldi_probe() == APP_PCB_PROBE_STATE_READY, "身份匹配的帧应被接受");
}

/* ================================================================
 *  RLS 探针
 * ================================================================ */

/** @brief RLS 探针：帧长域的上下界（0/1/下限-1/上限/上限+1）必须整段拦下
 *
 *  长度域是帧内 16 位值，低位侧（0/1）会通过 `data_len - 2` 向前越界读到帧头之前，
 *  高位侧会读到暂存区之外 —— 两侧都是 ASan 能抓的越界读，故逐一钉住。 */
static void test_rls(void)
{
    TEST_BEGIN("RLS 探针");

    /* 合法帧 */
    size_t n = rls_build(64);
    feed(s_frame, n);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_READY);
    CHECK(s_rls_len == 64);

    /* 数据不足 → WAIT */
    feed(s_frame, 8); /* 小于 sizeof(app_rls_frame_t) + 4 */
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_WAIT);

    /* 帧头错 → FAKE */
    rls_build(64);
    s_frame[0] = 0x00;
    feed(s_frame, n);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 帧尾错 → FAKE */
    rls_build(64);
    s_frame[62] = 0x00;
    feed(s_frame, n);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域超上限 → FAKE（此前会按 data_len 向后索引，越界读）*/
    rls_build(64);
    s_frame[2] = 0x02; /* 长度改成 0x02xx */
    s_frame[3] = 0x20;
    feed(s_frame, 64);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域 = 0 → FAKE（此前 data_len-2 向前越界，且会让框架零进度空转）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x00;
    feed(s_frame, 64);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域 = 1 → FAKE（下边界，data_len-2 仍会向前越界）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x01;
    feed(s_frame, 64);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 长度域 = 帧长下限 -1 → FAKE（RLS_FRAME_MIN = sizeof(app_rls_frame_t) + 3 = 9）*/
    rls_build(64);
    s_frame[2] = 0x00;
    s_frame[3] = 0x08;
    feed(s_frame, 64);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);

    /* 边界：恰好上限 → READY */
    n = rls_build(TEST_RLS_FRAME_MAX);
    feed(s_frame, n);
    CHECK_MSG(rls_probe() == APP_PCB_PROBE_STATE_READY, "上限边界应被接受（%u 字节）",
              (unsigned)TEST_RLS_FRAME_MAX);

    /* 上限 +1 → FAKE */
    n = rls_build(TEST_RLS_FRAME_MAX + 1);
    feed(s_frame, n);
    CHECK(rls_probe() == APP_PCB_PROBE_STATE_FAKE);
}

/* ================================================================
 *  暂存区容量契约
 * ================================================================ */

/** @brief 暂存区容量契约：装不下时必须 SKIP（并报出整帧长度），绝不能按帧内长度拷进去
 *
 *  暂存区容量由框架给（scratch_size），探针不能用"缓冲区可读量"去代替它 ——
 *  后者大于暂存区时就是写穿框架那份静态缓冲（ASan 抓 global-buffer-overflow）。 */
static void test_scratch_contract(void)
{
    TEST_BEGIN("暂存区容量契约：装不下时返回 SKIP 且不越界");

    uint32_t len = 0;
    uint8_t aux  = 0;

    /* IAP：给一个肯定装不下的暂存区 */
    size_t n = iap_build(0x05, 8);
    feed(s_frame, n);
    app_pcb_probe_state_t st =
        app_iap_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, 16, &len, &aux); /* 16 < 帧长 */
    CHECK(st == APP_PCB_PROBE_STATE_SKIP);
    CHECK(len == n);

    /* LDI 同理 */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    feed(s_frame, n);
    st = app_ldi_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, 16, &len, &aux);
    CHECK(st == APP_PCB_PROBE_STATE_SKIP);
    CHECK(len == n);

    /* RLS 同理 */
    n = rls_build(64);
    feed(s_frame, n);
    st = app_rls_probe_frame(&s_pcb, &s_ccb, nullptr, s_scratch_buf, 16, &len, &aux);
    CHECK(st == APP_PCB_PROBE_STATE_SKIP);
    CHECK(len == n);
}

/* ================================================================
 *  伪帧不得拷整帧（O(n²) 的守门）
 *
 *  框架对伪帧的做法是"跳 1 字节再探"，所以探针每次多拷一字节，整条路就退化成
 *  O(n²) 的 memcpy。级联那边实测：一条 1427 字节的杂物要拷 ~2MB，三个协议绑在
 *  同一条 485 上 ~6MB —— 在 168MHz 上是**四十多毫秒**，把帧分发任务按在那里，
 *  而这段时间里后到的帧会把前一条**完整但还没轮到解析**的帧从协议缓冲里挤掉
 *  （`app_ccb_dispatch` 装不下就"丢旧留新"），现场表现是"跨回绕拆成两段的帧总丢"。
 *
 *  做法：给一大段"一个真帧头都没有"的杂物，暂存区铺哨兵，跑完检查**帧头那几个字节
 *  以外**原封不动。判"是不是本协议的帧"最多只需要读到自己的长度域，所以取一个宽松
 *  上界 32 字节 —— 比 1440 小两个数量级，足以抓住"拷了整帧"这一类回归。
 *
 *  **反向验证**：把任一探针改回按**缓冲区可读量**（而不是按暂存区容量 scratch_size）
 *  去夹紧 `rb_peek` 的 dest_cap，本用例红。
 * ================================================================ */

/** @brief 伪帧时不得把整帧拷进暂存区（否则"跳 1 字节重试"退化成 O(n²) 的 memcpy）
 *
 *  判"是不是本协议的帧"最多只需读自己的帧头+长度域（≤32 字节），把 1400 字节的
 *  杂物整帧拷走会把帧分发任务拖住几十毫秒，期间下游协议缓冲被写满会挤掉完整帧。 */
static void test_fake_does_not_copy_whole(void)
{
    TEST_BEGIN("伪帧时不得拷整帧（否则逐字节重跳是 O(n²)，实测拖住 40ms+）");

    enum { HDR_MAX = 32 }; /* 判帧头+长度域所需的上界，远小于整帧 */
    static uint8_t junk[1400];

    static const struct {
        const char     *name;
        app_pcb_probe_fn_t  probe;
    } t[] = {
        {"iap", app_iap_probe_frame},
        {"ldi", app_ldi_probe_frame},
        {"rls", app_rls_probe_frame},
    };

    memset(junk, 0x43, sizeof(junk)); /* 一个 A5 5A / FF FE / 0x7E 都不给 */

    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        s_rb.read_index  = 0;
        s_rb.write_index = 0;
        feed(junk, (uint16_t)sizeof(junk));

        memset(s_scratch_buf, 0xEE, sizeof(s_scratch_buf));
        uint32_t        len = 0;
        uint8_t         aux = 0;
        app_pcb_probe_state_t st  = t[i].probe(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf),
                                         &len, &aux);

        CHECK_MSG(st != APP_PCB_PROBE_STATE_READY, "%s：这段杂物不该判 READY（得到 %u）", t[i].name,
                  (unsigned)st);

        bool untouched = true;
        for (uint16_t k = HDR_MAX; k < sizeof(s_scratch_buf); k++) {
            if (s_scratch_buf[k] != 0xEE) {
                untouched = false;
                break;
            }
        }
        CHECK_MSG(untouched,
                  "%s: FAKE 却把整帧拷进了暂存区 —— 逐字节重跳会退化成 O(n²) 的 memcpy，"
                  "把帧分发任务拖住几十毫秒，下游协议缓冲在那期间被写满会冲掉完整帧",
                  t[i].name);
    }
}

/* ================================================================
 *  FAKE 的处置契约：伪帧头恰好前进 1 字节
 *
 *  app_dispatch.h 的四态定义："FAKE = 伪帧头（如误匹配的 0x5A/0xFF），跳过 1 字节重试"。
 *  探针自身只窥视（rb_peek 不消费），"前进 1 字节"由框架在拿到 FAKE 后执行 ——
 *  所以这里把框架的动作照抄一遍（rb_skip 1），断言**这一轮的总前进量恰好 1 字节**，
 *  再断言紧跟在伪字节后的合法帧立刻被认出：
 *    · 少跳（0 字节）→ 原地空转，每次重跳都重拷同一段（O(n²)，同上一条用例）；
 *    · 多跳（2 字节）→ 恰好跨过真帧的第一个字节，真帧永远认不出来
 *      （现场症状："跨回绕拆成两段的帧总丢"）。
 *  三种协议各测一遍：契约在框架侧共享，任何一家探针偷偷消费一个字节都会让前进量变 2。
 * ================================================================ */

/** @brief 喂 [1 个伪字节 + 一条合法帧]，跑一轮"探针 + 框架处置"，断言前进量恰好 1 字节 */
static void fake_advance_check(const char *name, app_pcb_probe_fn_t probe, const uint8_t *frame,
                               size_t frame_len, uint32_t *got_len)
{
    static uint8_t s_one = 0x00; /* 伪字节：帧头首字节失效是 FAKE 的典型来源 */
    uint8_t        aux   = 0;

    rb_flush(&s_rb, NULL);
    rb_write(&s_rb, &s_one, 1, NULL);
    rb_write(&s_rb, frame, (uint16_t)frame_len, NULL);

    uint16_t avail = rb_avail(&s_rb, NULL);
    CHECK_MSG(probe(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), got_len, &aux) ==
                  APP_PCB_PROBE_STATE_FAKE,
              "%s：首字节错开的合法帧应判 FAKE", name);

    /* 框架对 FAKE 的处置（app_dispatch.c：skip = 1）*/
    rb_skip(&s_rb, 1, NULL);
    CHECK_MSG((uint16_t)(avail - rb_avail(&s_rb, NULL)) == 1U,
              "%s：FAKE 处置后应恰好前进 1 字节（得到 %u）—— 探针自己消费 / 框架多跳都会 >1", name,
              (unsigned)(avail - rb_avail(&s_rb, NULL)));

    /* 前进 1 之后必须立刻认出紧邻的合法帧：多跳 1 字节会把帧头也跨过去 */
    *got_len = 0;
    CHECK_MSG(probe(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), got_len, &aux) ==
                      APP_PCB_PROBE_STATE_READY &&
                  *got_len == frame_len,
              "%s：跳 1 字节后应立即认出紧跟的合法帧（得到 len=%u，期望 %u）", name,
              (unsigned)*got_len, (unsigned)frame_len);
}

/** @brief 三种协议的 FAKE 前进量都恰好 1 字节（多跳丢帧、少跳空转；探针偷消费会让它变 2） */
static void test_fake_advances_exactly_one_byte(void)
{
    TEST_BEGIN("FAKE 处置：伪帧头恰好前进 1 字节（多跳丢帧、少跳空转）");

    uint32_t len = 0;

    size_t n = iap_build(0x05, 4);
    fake_advance_check("iap", app_iap_probe_frame, s_frame, n, &len);

    /* 这里用配置指令（免身份校验）—— 本用例不测身份分支，换别的主角请先看 test_ldi */
    n = ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20);
    fake_advance_check("ldi", app_ldi_probe_frame, s_frame, n, &len);

    n = rls_build(64);
    fake_advance_check("rls", app_rls_probe_frame, s_frame, n, &len);
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
    app_pcb_probe_fn_t  probe;
    uint16_t          payload_max;
    size_t          (*build)(uint32_t variant); /**< 在 s_frame 里造一帧，返回长度 */
} probe_entry_t;

static size_t build_iap(uint32_t v)
{
    return iap_build(0x05, v % 17);
}
static size_t build_ldi(uint32_t v)
{
    return ldi_build(APP_LDI_CMD_TYPE_SET_IP_REQ, 20 + (v % 64));
}
static size_t build_rls(uint32_t v)
{
    return rls_build((uint16_t)(16 + (v % 120)));
}

static const probe_entry_t s_probes[] = {
    {"iap", app_iap_probe_frame, 1044, build_iap},
    {"ldi", app_ldi_probe_frame, 522, build_ldi},
    {"rls", app_rls_probe_frame, 530, build_rls},
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
        app_pcb_probe_state_t st =
            pe->probe(&s_pcb, &s_ccb, nullptr, s_scratch_buf, sizeof(s_scratch_buf), &total, &aux);

        if (st == APP_PCB_PROBE_STATE_WAIT) {
            CHECK_MSG(rb_avail(&s_rb, NULL) == avail, "%s: WAIT 消费了数据", pe->name);
            return; /* 等更多数据，此处没有了 */
        }

        if (st == APP_PCB_PROBE_STATE_READY || st == APP_PCB_PROBE_STATE_SKIP) {
            CHECK_MSG(total > 0, "%s: %s 返回帧长 0（会让框架零进度空转）", pe->name,
                      st == APP_PCB_PROBE_STATE_READY ? "READY" : "SKIP");
            CHECK_MSG(total <= avail, "%s: %s 声明帧长 %u 超过可用字节 %u", pe->name,
                      st == APP_PCB_PROBE_STATE_READY ? "READY" : "SKIP", total, avail);
            if (st == APP_PCB_PROBE_STATE_READY)
                CHECK_MSG(total <= pe->payload_max, "%s: READY 帧长 %u 超过 payload_max %u",
                          pe->name, total, pe->payload_max);
        }

        uint16_t skip = 0;
        if (st == APP_PCB_PROBE_STATE_READY || st == APP_PCB_PROBE_STATE_SKIP)
            skip = (uint16_t)total;
        else
            skip = 1; /* FAKE */

        if (rb_skip(&s_rb, skip, NULL) == 0) return; /* 数据不足，收敛 */
    }
}

/** @brief 通用契约（随机 + 构造字节流）：只断言四条不变量，覆盖"没想到的地方"
 *
 *  不变量 = READY/SKIP 帧长为正且 ≤ payload_max、≤ 当前可用字节；WAIT 不消费。
 *  新增协议只要加一行 s_probes 表项就自动受检，不依赖"记得写用例"
 *  —— 越界读写由 ASan 直接抓，不必写成断言。 */
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
    test_fake_does_not_copy_whole();
    test_fake_advances_exactly_one_byte();
    test_conformance();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
