/**
 * @file    test_dispatch.c
 * @brief   协议/通道分发引擎 —— host 单测
 *
 * 用假通道 + 可脚本化探针驱动**真实的** app_dispatch_task（跑在 pthread 上）：
 * cmsis_os2 由 test/stubs/os_stub.c 用 pthread 实现，app_dispatch.c / ring_buffer.c
 * 一行都不用为测试改动，因此测的是生产代码本身而不是它的复制品。
 *
 * 编译开 ASan + UBSan —— 这恰恰覆盖了此前几个探针缺陷的类别（写进固定数组时
 * 不管目标容量、长度域未校验就索引）。
 *
 * 来源：从参考工程 Project_STD_B/test/test_dispatch.c 移植。本工程的分发框架
 * （Application/Inc/app_dispatch.h + Application/Src/app_dispatch.c）与该工程
 * 逐字节等价，故本文件除本段注释外未作改动 —— 两边跑的是同一套用例。
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "FreeRTOS.h" /* StaticQueue_t（与各协议模块的用法一致）*/
#include "app_dispatch.h"

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
 *  假协议 —— 派生结构体，base 必须是第一个成员
 *
 *  这同时是"约定 1"的回归用例：探针只能通过 container_of(self, ...) 拿回
 *  自己的脚本与统计，找不到就说明基类布局约定被破坏了。
 * ================================================================ */

/* RB 必须大于 FRAME_DATA_MAX_LEN：否则 avail 天然小于暂存区，
   "超长 READY 触发暂存区越界"这条永远测不出来 */
#define FAKE_RB_SIZE (2048U)
#define FAKE_PAYLOAD_MAX (128U)
#define FAKE_QUEUE_DEPTH (8U)
#define FAKE_MSG_SIZE (sizeof(app_dispatch_msg_t) + FAKE_PAYLOAD_MAX)
#define SCRIPT_MAX (64)

typedef struct {
    app_pcb_probe_state_t state;
    uint32_t          len;
    uint8_t           aux; /**< 探针输出的协议层分类，用于验证逐帧传递 */
} script_step_t;

typedef struct {
    app_pcb_t            base; /**< 第一个成员：container_of 还原 */
    script_step_t    script[SCRIPT_MAX];
    int              script_len;
    int              script_pos;
    int              probe_calls;
    app_pcb_t           *seen_self;
    const app_ccb_t *seen_ccb;
    /* src 只在本次调用内有效 —— 假探针按契约当场消费（拷走主题），不留指针 */
    bool             seen_src_valid;
    char             seen_src_topic[APP_CCB_SRC_TOPIC_MAX];
    uint16_t         seen_scratch_size;
    ring_buffer_t      rb;
    uint8_t            rb_buf[FAKE_RB_SIZE];
    osMessageQueueId_t queue;
    StaticQueue_t      queue_cb;
    uint8_t            queue_buf[FAKE_QUEUE_DEPTH * FAKE_MSG_SIZE];
} fake_proto_t;

static fake_proto_t s_pa;
static fake_proto_t s_pb;

static app_pcb_probe_state_t fake_probe(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                    uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                    uint8_t *aux)
{
    (void)scratch;
    fake_proto_t *p = container_of(self, fake_proto_t, base);

    /* 饱和计数：空转时不要把自己的计数器溢出变成 UBSan 报错，
       真正的故障应由看门狗以"超时"的形式报出来 */
    if (p->probe_calls < 1000000) p->probe_calls++;
    p->seen_self         = self;
    p->seen_ccb           = ccb;
    p->seen_src_valid    = (src != nullptr && src->topic != nullptr);
    if (p->seen_src_valid) {
        strncpy(p->seen_src_topic, src->topic, sizeof(p->seen_src_topic) - 1);
        p->seen_src_topic[sizeof(p->seen_src_topic) - 1] = '\0';
    }
    p->seen_scratch_size = scratch_size;

    script_step_t st = {.state = APP_PCB_PROBE_STATE_WAIT, .len = 0, .aux = 0};
    if (p->script_pos < p->script_len) {
        st = p->script[p->script_pos++];
    } else if (p->script_len > 0) {
        st = p->script[p->script_len - 1]; /* 脚本用完后重复最后一步 */
    }

    *total_len = st.len;
    *aux       = st.aux;
    return st.state;
}

static const app_pcb_ops_t s_fake_ops = {.probe = fake_probe};

/* ================================================================
 *  假通道
 * ================================================================ */

static int             s_send_calls;
static uint16_t        s_send_len;
static const app_ccb_dst_t *s_send_dst;

static int32_t fake_send(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    (void)data;
    s_send_calls++;
    s_send_len = len;
    s_send_dst = dst;
    return (int32_t)len;
}

static const app_ccb_ops_t s_chan_ops = {.send = fake_send};

static app_ccb_t s_chan_a = {.name = "chan_a", .ops = &s_chan_ops};
static app_ccb_t s_chan_b = {.name = "chan_b", .ops = &s_chan_ops};

/* ================================================================
 *  装置
 * ================================================================ */

static void script_begin(fake_proto_t *p)
{
    p->script_len = 0;
    p->script_pos = 0;
}

static void script_with_aux(fake_proto_t *p, app_pcb_probe_state_t state, uint32_t len, uint8_t aux)
{
    if (p->script_len < SCRIPT_MAX)
        p->script[p->script_len++] = (script_step_t){.state = state, .len = len, .aux = aux};
}

static void script(fake_proto_t *p, app_pcb_probe_state_t state, uint32_t len)
{
    script_with_aux(p, state, len, 0x5A); /* 既有用例统一用这个分类值 */
}

static void proto_reset(fake_proto_t *p, const char *name)
{
    script_begin(p);
    p->probe_calls       = 0;
    p->seen_self         = NULL;
    p->seen_ccb           = NULL;
    p->seen_src_valid    = false;
    p->seen_src_topic[0] = '\0';
    p->seen_scratch_size = 0;

    /* 持锁清空：分发任务可能正在这一轮排空里 */
    rb_flush(&p->rb, p->rb.mutex);

    osMessageQueueAttr_t qa = {
        .name    = name,
        .cb_mem  = &p->queue_cb,
        .cb_size = sizeof(p->queue_cb),
        .mq_mem  = p->queue_buf,
        .mq_size = sizeof(p->queue_buf),
    };
    p->queue      = osMessageQueueNew(FAKE_QUEUE_DEPTH, FAKE_MSG_SIZE, &qa);
    p->base.queue = p->queue;
}

/** @brief 一次性构造（main 里调用），绑定缓冲区并建互斥量 */
static void proto_create(fake_proto_t *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    p->rb.data = p->rb_buf;
    p->rb.size = FAKE_RB_SIZE;
    rb_init(&p->rb, name);

    p->base = (app_pcb_t){
        .name        = name,
        .ops         = &s_fake_ops,
        .rb          = &p->rb,
        .payload_max = FAKE_PAYLOAD_MAX,
    };
    proto_reset(p, name);
}

static void chan_reset(app_ccb_t *ccb, const char *name)
{
    ccb->name      = name;
    ccb->ops       = &s_chan_ops;
    ccb->state     = APP_CCB_STATE_UP;
    ccb->proto_cnt = 0;
}

static void scenario_begin(void)
{
    chan_reset(&s_chan_a, "chan_a");
    chan_reset(&s_chan_b, "chan_b");
    proto_reset(&s_pa, "pa");
    proto_reset(&s_pb, "pb");
}

/* ---- 收帧 ---- */

static uint8_t s_rx[FAKE_MSG_SIZE] __attribute__((aligned(4)));
#define RX ((app_dispatch_msg_t *)s_rx)

static bool wait_frame(fake_proto_t *p, uint32_t timeout_ms)
{
    memset(s_rx, 0, sizeof(s_rx));
    return osMessageQueueGet(p->queue, s_rx, NULL, timeout_ms) == osOK;
}

static void feed(const app_ccb_t *ccb, const void *data, size_t len)
{
    app_ccb_dispatch(ccb, NULL, (const uint8_t *)data, (uint16_t)len);
}

static uint16_t rb_used(fake_proto_t *p)
{
    return rb_avail(&p->rb, p->rb.mutex);
}

/* ================================================================
 *  用例
 * ================================================================ */

static void test_single_frame(void)
{
    TEST_BEGIN("单帧 READY：帧内容、来源通道与探针契约");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 5);
    feed(&s_chan_a, "HELLO", 5);

    CHECK(wait_frame(&s_pa, 500));
    CHECK(RX->data_len == 5);
    CHECK(memcmp(RX->data, "HELLO", 5) == 0);
    CHECK(RX->ccb == &s_chan_a);

    /* 探针必须收到自己的 pcb（派生协议靠它 container_of 回自身属性）*/
    CHECK(s_pa.seen_self == &s_pa.base);
    CHECK(s_pa.seen_ccb == &s_chan_a);
    CHECK(s_pa.seen_scratch_size == FRAME_DATA_MAX_LEN);
    CHECK(RX->aux == 0x5A); /* 探针分类随消息投递 */
}

static void test_multi_frame_drain(void)
{
    TEST_BEGIN("一次唤醒抽出多帧：while (avail > 0) 连续解析");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 4); /* 脚本用完后重复 → 连抽 */
    feed(&s_chan_a, "AAAABBBBCCCC", 12);

    const char *expect[3] = {"AAAA", "BBBB", "CCCC"};
    for (int i = 0; i < 3; i++) {
        CHECK(wait_frame(&s_pa, 500));
        CHECK(RX->data_len == 4);
        CHECK(memcmp(RX->data, expect[i], 4) == 0);
    }
    CHECK(!wait_frame(&s_pa, 100)); /* 不应有多余帧 */
    CHECK(s_pa.probe_calls == 3);
}

static void test_fake_skips_one_byte(void)
{
    TEST_BEGIN("FAKE：只跳 1 字节后重新同步");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_FAKE, 0);
    script(&s_pa, APP_PCB_PROBE_STATE_FAKE, 0);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 3);
    feed(&s_chan_a, "xxABC", 5);

    CHECK(wait_frame(&s_pa, 500));
    CHECK(RX->data_len == 3);
    CHECK(memcmp(RX->data, "ABC", 3) == 0);
    CHECK(s_pa.probe_calls == 3);
}

static void test_skip_skips_whole_frame(void)
{
    TEST_BEGIN("SKIP：整帧跳过（不是 1 字节）");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_SKIP, 3);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 2);
    feed(&s_chan_a, "XYZPQ", 5);

    CHECK(wait_frame(&s_pa, 500));
    CHECK(RX->data_len == 2);
    CHECK(memcmp(RX->data, "PQ", 2) == 0);
}

static void test_wait_consumes_nothing(void)
{
    TEST_BEGIN("WAIT：不消费数据，跨通知累积成帧");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_WAIT, 0);
    feed(&s_chan_a, "AB", 2);
    CHECK(!wait_frame(&s_pa, 100));
    CHECK_MSG(rb_used(&s_pa) == 2, "WAIT 后缓冲区应仍留有 2 字节，实际 %u", rb_used(&s_pa));

    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 5);
    feed(&s_chan_a, "CDE", 3);

    CHECK(wait_frame(&s_pa, 500));
    CHECK(RX->data_len == 5);
    CHECK(memcmp(RX->data, "ABCDE", 5) == 0);
}

static void test_zero_length_guard(void)
{
    TEST_BEGIN("零长度 READY：不投递空帧，且不零进度死循环");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 0);
    feed(&s_chan_a, "AB", 2);

    CHECK(!wait_frame(&s_pa, 150));
    CHECK(rb_used(&s_pa) == 0); /* 逐字节丢弃，但确有推进 */

    /* 死循环会让任务永远回不到队列等待，下面的帧就收不到 */
    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 2);
    feed(&s_chan_a, "CD", 2);
    CHECK_MSG(wait_frame(&s_pa, 500), "任务疑似卡死：后续帧未送达");
    CHECK(memcmp(RX->data, "CD", 2) == 0);
}

static void test_payload_max_guard(void)
{
    TEST_BEGIN("超长 READY：被 payload_max 拒绝，且不越界读写");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    static uint8_t big[FAKE_RB_SIZE];
    memset(big, 0xAA, sizeof(big));

    script(&s_pa, APP_PCB_PROBE_STATE_READY, FAKE_PAYLOAD_MAX + 1);
    feed(&s_chan_a, big, sizeof(big));

    CHECK(!wait_frame(&s_pa, 200));

    /* 极端值：远超框架暂存区（未加守卫时 rb_read 会写穿 _msg_dispatch_buf，
       由 ASan 抓 global-buffer-overflow）*/
    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, FRAME_DATA_MAX_LEN + 500);
    feed(&s_chan_a, big, sizeof(big));
    CHECK(!wait_frame(&s_pa, 200));

    /* 引擎仍须可用 */
    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 3);
    feed(&s_chan_a, "OK!", 3);
    CHECK(wait_frame(&s_pa, 500));
    CHECK(memcmp(RX->data, "OK!", 3) == 0);
}

static void test_two_protocols_one_channel(void)
{
    TEST_BEGIN("同一通道绑定两个协议：各自独立缓冲区与队列");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    app_dispatch_bind(&s_pb.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 4);
    script(&s_pb, APP_PCB_PROBE_STATE_READY, 4);
    feed(&s_chan_a, "DATA", 4);

    CHECK(wait_frame(&s_pa, 500));
    CHECK(RX->data_len == 4 && memcmp(RX->data, "DATA", 4) == 0);
    CHECK(wait_frame(&s_pb, 500));
    CHECK(RX->data_len == 4 && memcmp(RX->data, "DATA", 4) == 0);
}

static void test_channel_isolation(void)
{
    TEST_BEGIN("通道隔离：数据只进绑定通道的协议缓冲区");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 4);
    feed(&s_chan_b, "DATA", 4); /* pa 没绑 chan_b */

    CHECK(!wait_frame(&s_pa, 150));
    CHECK_MSG(rb_used(&s_pa) == 0, "未绑定通道的数据不应进入缓冲区，实际 %u", rb_used(&s_pa));
    CHECK(s_pa.probe_calls == 0);
}

static void test_bind_idempotent(void)
{
    TEST_BEGIN("重复绑定同一 (协议, 通道) 不产生重复条目");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    CHECK(s_chan_a.proto_cnt == 1);
}

static int s_listener_calls;

static void rx_listener(void)
{
    s_listener_calls++;
}

/** 接收事件监听 —— 语义是"**上位机下发了一条指令**"（工厂模式据此退出）
 *
 *  两条边界都是现场踩出来的，不是推演出来的：
 *   · **半帧不算**：字节到了、帧没齐 → 不触发（调用方拿到的必须是"一条指令"）；
 *   · **设备内部总线不算**：级联的 PRESENT/ACK 在老化测试期间每轮都有，
 *     让它们触发监听会把正在跑的工厂测试反复打断 —— 现场表现就是
 *     "按第一下之后测试再也推不动"。
 *
 *  反向验证：把监听点退回 `app_ccb_dispatch`（收到字节就通知），
 *  或去掉 `!p->internal_bus` 那个条件，③ 会立刻红。 */
static void test_rx_listener(void)
{
    TEST_BEGIN("接收事件监听：只在**非内部总线协议的有效帧**上触发");

    /* **同步点变了**：通知发生在帧分发任务里（收到整帧之后），不再是
       `app_ccb_dispatch` 里同步做的 —— 所以每条都要先 `wait_frame` 等到那一帧
       投递出来，再断言监听次数。少了这一步，断言会在任务还没跑到时就下结论。 */

    /* ① 外部协议、一整帧 → 触发一次 */
    scenario_begin();
    s_pa.base.internal_bus = false;
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);

    s_listener_calls = 0;
    feed(&s_chan_a, "Z", 1);
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_listener_calls == 1, "一整帧应触发一次，得到 %d", s_listener_calls);

    /* ② 半帧（WAIT）不算，补齐成整帧才算 —— 半帧那一下**不许**提前通知 */
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    script(&s_pa, APP_PCB_PROBE_STATE_WAIT, 0);

    s_listener_calls = 0;
    feed(&s_chan_a, "A", 1); /* 只有半帧 */

    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 2);
    feed(&s_chan_a, "B", 1); /* 补齐 */
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_listener_calls == 1, "半帧不该通知、整帧才通知：共应 1 次，得到 %d",
              s_listener_calls);

    /* ③ 设备内部总线（级联）：整帧也不触发 */
    scenario_begin();
    s_pa.base.internal_bus = true;
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);

    s_listener_calls = 0;
    feed(&s_chan_a, "ZZ", 2);
    CHECK(wait_frame(&s_pa, 500)); /* 两帧都投递出来（同步点） */
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_listener_calls == 0, "内部总线协议的帧不该触发监听，得到 %d", s_listener_calls);
    s_pa.base.internal_bus = false; /* 还原，别影响后面的用例 */

    /* ④ 伪帧（FAKE）：跳 1 字节重试，谁都还没收下 → 不触发 */
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    script(&s_pa, APP_PCB_PROBE_STATE_FAKE, 0);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);

    s_listener_calls = 0;
    feed(&s_chan_a, "xZ", 2); /* 先一个伪字节，再一整帧 */
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_listener_calls == 1, "伪帧不该通知，只有后面那整帧算：应 1 次，得到 %d",
              s_listener_calls);

    /* ⑤ 一批里的两条帧 → 两次（**每帧**一次，不是每批一次） */
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 1);

    s_listener_calls = 0;
    feed(&s_chan_a, "ZZ", 2);
    CHECK(wait_frame(&s_pa, 500));
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_listener_calls == 2, "两条帧应触发两次，得到 %d", s_listener_calls);
}

static void test_ccb_send(void)
{
    TEST_BEGIN("app_ccb_send：虚表分派、目的地透传与空守卫");
    scenario_begin();

    s_send_calls = 0;
    s_send_dst   = (const app_ccb_dst_t *)&s_chan_a; /* 非空哨兵：确认被改写 */

    app_ccb_send(&s_chan_a, (const uint8_t *)"AB", 2);
    CHECK(s_send_calls == 1);
    CHECK(s_send_len == 2);
    CHECK_MSG(s_send_dst == NULL, "app_ccb_send 应传 nullptr（回复到本帧来源）");

    const app_ccb_dst_t dst = {.broadcast = true, .topic = "some/topic"};
    app_ccb_send_to(&s_chan_a, &dst, (const uint8_t *)"CD", 2);
    CHECK(s_send_calls == 2);
    CHECK(s_send_dst == &dst); /* 目的地原样传到通道实现，由它自行解释 */
    if (s_send_dst != NULL) {  /* 先判空：断言失败时不要在这里解引用崩掉 */
        CHECK(s_send_dst->broadcast);
        CHECK(s_send_dst->topic != NULL);
    }

    app_ccb_send(NULL, (const uint8_t *)"AB", 2); /* 不应崩溃 */
    app_ccb_t broken = {.name = "broken", .ops = NULL};
    app_ccb_send(&broken, (const uint8_t *)"AB", 2); /* 不应崩溃 */
    CHECK(s_send_calls == 2);
}

static void test_write_overflow_policy(void)
{
    TEST_BEGIN("缓冲区装不下时：丢旧留新，不留半截帧");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    /* 探针恒返回 WAIT（不消费），便于直接观察缓冲区里到底留下了什么 */
    script(&s_pa, APP_PCB_PROBE_STATE_WAIT, 0);

    static uint8_t chunk1[1500];
    static uint8_t chunk2[1000];
    memset(chunk1, 0x11, sizeof(chunk1));
    memset(chunk2, 0x22, sizeof(chunk2));

    feed(&s_chan_a, chunk1, sizeof(chunk1));
    osDelay(50);
    CHECK_MSG(rb_used(&s_pa) == sizeof(chunk1), "首次写入应全部落下，实际 %u", rb_used(&s_pa));

    /* 剩余空间（2048-1500-1 = 547）不足以容纳 chunk2 → 应整段丢弃旧数据后写入 */
    feed(&s_chan_a, chunk2, sizeof(chunk2));
    osDelay(50);

    CHECK_MSG(rb_used(&s_pa) == sizeof(chunk2), "装不下时应丢旧留新（期望 %u 字节，实际 %u）",
              (unsigned)sizeof(chunk2), rb_used(&s_pa));

    uint8_t head[16] = {0};
    rb_peek(&s_pa.rb, 0, head, sizeof(head), s_pa.rb.mutex);
    CHECK(head[0] == 0x22 && head[15] == 0x22); /* 留下的是新数据，不是半截旧帧 */
}

static void test_src_passthrough(void)
{
    TEST_BEGIN("接收来源随通知带到探针（协议据此分类，无需认识通道类型）");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    /* 调用方缓冲在派发后被改写，模拟通道随即收到下一条消息 */
    static char caller_buf[APP_CCB_SRC_TOPIC_MAX];
    strcpy(caller_buf, "ASK/one");
    const app_ccb_src_t src = {.topic = caller_buf};

    script(&s_pa, APP_PCB_PROBE_STATE_READY, 2);
    app_ccb_dispatch(&s_chan_a, &src, (const uint8_t *)"AB", 2);
    strcpy(caller_buf, "ASK/two"); /* 立刻覆盖：框架应已拷贝，不受影响 */

    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(s_pa.seen_src_valid, "探针应收到来源描述");
    CHECK_MSG(strcmp(s_pa.seen_src_topic, "ASK/one") == 0,
              "调用方缓冲被覆盖后，探针仍应看到派发时的取值，实际 '%s'", s_pa.seen_src_topic);

    /* 无来源概念的通道（RS485/TCP/UDP）传 nullptr，探针需能处理 */
    script_begin(&s_pa);
    script(&s_pa, APP_PCB_PROBE_STATE_READY, 2);
    feed(&s_chan_a, "CD", 2);
    CHECK(wait_frame(&s_pa, 500));
    CHECK_MSG(!s_pa.seen_src_valid, "无来源时探针应收到 nullptr");
}

static void test_aux_propagation(void)
{
    TEST_BEGIN("探针分类经 app_dispatch_msg_t.aux 逐帧投递给协议任务");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    /* 三帧各带不同分类值 —— 验证是「逐帧携带」而不是「最后一次的值」，
       后者正是"把 per-message 的值寄存在长生命周期对象上"的典型症状 */
    script_with_aux(&s_pa, APP_PCB_PROBE_STATE_READY, 2, 0x11);
    script_with_aux(&s_pa, APP_PCB_PROBE_STATE_READY, 2, 0x22);
    script_with_aux(&s_pa, APP_PCB_PROBE_STATE_READY, 2, 0x33);
    feed(&s_chan_a, "AABBCC", 6);

    for (uint8_t i = 0; i < 3; i++) {
        CHECK(wait_frame(&s_pa, 500));
        uint8_t expect = (uint8_t)(0x11 * (i + 1));
        CHECK_MSG(RX->aux == expect, "第 %u 帧 aux 应为 %#04x，实际 %#04x", i, expect, RX->aux);
    }
}

static void test_empty_dispatch(void)
{
    TEST_BEGIN("边界入参：空指针 / 零长度不崩溃、不入队");
    scenario_begin();
    app_dispatch_bind(&s_pa.base, &s_chan_a);

    app_ccb_dispatch(NULL, NULL, (const uint8_t *)"X", 1);
    app_ccb_dispatch(&s_chan_a, NULL, NULL, 1);
    app_ccb_dispatch(&s_chan_a, NULL, (const uint8_t *)"X", 0);

    CHECK(!wait_frame(&s_pa, 100));
    CHECK(s_pa.probe_calls == 0);
}

/* ================================================================
 *  入口
 * ================================================================ */

/* ================================================================
 *  看门狗
 *
 *  引擎一旦零进度空转（曾经真实发生过的缺陷类别），分发任务会一直攥着 RB 锁，
 *  测试端 rb_flush 随之永久阻塞 —— 回归以"挂死"而非"失败"的形式暴露，
 *  这在 CI 里比失败更糟。用 SIGALRM 把挂死转成明确的失败退出。
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似引擎死循环/死锁）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("协议/通道分发引擎 host 单测\n");

    proto_create(&s_pa, "pa");
    proto_create(&s_pb, "pb");

    app_dispatch_init(); /* 建 ccb_queue 并启动真实的 app_dispatch_task */
    app_dispatch_register_rx_listener(rx_listener);

    test_single_frame();
    test_multi_frame_drain();
    test_fake_skips_one_byte();
    test_skip_skips_whole_frame();
    test_wait_consumes_nothing();
    test_zero_length_guard();
    test_payload_max_guard();
    test_two_protocols_one_channel();
    test_channel_isolation();
    test_bind_idempotent();
    test_rx_listener();
    test_ccb_send();
    test_aux_propagation();
    test_src_passthrough();
    test_write_overflow_policy();
    test_empty_dispatch();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
