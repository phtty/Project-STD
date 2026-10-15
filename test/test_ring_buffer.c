/**
 * @file    test_ring_buffer.c
 * @brief   ring_buffer 窥视路径的 host 单测（重点是 rb_peek 的容量夹紧）
 *
 * 为什么需要这个测试：窥视越界写是"在硬件上极难构造、在现场表现为偶尔有点怪"
 * 的一类缺陷——溢出落在相邻 .bss 变量上，不崩溃、不报错。LDI 与 RLS 的探针
 * 曾用**旧契约**的 `rb_peek(buff, 0, mem_pool, avail, nullptr)` 把最多 2047 字节写进
 * 512 字节的栈数组——那版契约的 `len` 只按缓冲区自身容量夹紧、根本不知道 dest 有多大。
 * 旧契约已被废除：现在的 `rb_peek` 第 4 个参数是 `dest_cap`（硬上限），本题用 ASan
 * 的红区把越界变成必现的失败。
 *
 * 内容比对不用手推偏移，而是维护一个**影子模型**：按入队顺序把字节记进平铺数组，
 * 于是"rb 里应该读到什么"就是 shadow[rd..wr) —— 手推环形偏移极易算错，
 * 而算错的期望值会把真缺陷掩盖成假失败。
 *
 * 被测代码是生产源码本体（Kernel/Src/ring_buffer.c），不做任何替换。
 *
 * 构建与运行见 Makefile 的 test 目标。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ring_buffer.h"

/* ---- 极简断言 ---- */
static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, fmt, ...)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);                    \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* ---- 被测缓冲区 ---- */
#define TEST_RB_SIZE (2048U)
RB_DEFINE(s_rb, TEST_RB_SIZE);

/* ---- 影子模型 ---- */
#define SHADOW_CAP (TEST_RB_SIZE * 4)
static uint8_t s_shadow[SHADOW_CAP];
static uint16_t s_shadow_rd, s_shadow_wr;

static void shadow_reset(void)
{
    s_shadow_rd = 0;
    s_shadow_wr = 0;
}

static uint16_t shadow_avail(void)
{
    return (uint16_t)(s_shadow_wr - s_shadow_rd);
}

/** @brief 把 rb 当前应读到的 avail 字节与影子模型比对 */
static void check_contents_match_shadow(const char *what)
{
    uint16_t avail = rb_avail(&s_rb, nullptr);
    if (avail != shadow_avail()) {
        printf("  FAIL %s: rb avail=%u 但影子模型 %u\n", what, avail, shadow_avail());
        g_failures++;
        return;
    }
    if (avail == 0) return;

    uint8_t *dest = malloc(avail);
    uint16_t got  = rb_peek(&s_rb, 0, dest, avail, nullptr);
    if (got != avail) {
        printf("  FAIL %s: 窥视返回 %u 应为 %u\n", what, got, avail);
        g_failures++;
        free(dest);
        return;
    }
    int mismatch = -1;
    for (uint16_t i = 0; i < avail; i++)
        if (dest[i] != s_shadow[s_shadow_rd + i]) {
            mismatch = i;
            break;
        }
    if (mismatch >= 0)
        printf("  FAIL %s: 第 %d 字节 = 0x%02X 应为 0x%02X\n", what, mismatch, dest[mismatch],
               s_shadow[s_shadow_rd + mismatch]);
    if (mismatch >= 0) g_failures++;
    free(dest);
}

/** @brief 写入 n 字节递增模式，同步进影子模型；返回实际写入量 */
static uint16_t rb_write_pattern(uint16_t n)
{
    uint8_t *tmp = malloc(n);
    for (uint16_t i = 0; i < n; i++)
        tmp[i] = (uint8_t)((s_shadow_wr + i) & 0xFF);

    uint16_t w = rb_write(&s_rb, tmp, n, nullptr);
    memcpy(&s_shadow[s_shadow_wr], tmp, w);
    s_shadow_wr += w;
    free(tmp);
    return w;
}

/** @brief 读掉 n 字节，同步影子模型 */
static uint16_t rb_read_pattern(uint16_t n)
{
    uint8_t *sink = malloc(n);
    uint16_t r    = rb_read(&s_rb, sink, n, nullptr);
    s_shadow_rd += r;
    free(sink);
    return r;
}

static void reset_all(void)
{
    rb_flush(&s_rb, nullptr);
    shadow_reset();
}

/* ================================================================
 *  用例
 * ================================================================ */

/**
 * @brief 核心性质：dest_cap 小于可窥视量时，只拷 dest_cap 字节且不越界
 *
 * 这正是 LDI/RLS 探针踩的那条路径：缓冲区里累积了远多于 512 字节的数据，
 * 目标只有 512。dest 走 malloc 以便 ASan 在其后布置红区——多写一个字节都会被抓住。
 */
static void case_capped_never_exceeds_dest(void)
{
    printf("case_capped_never_exceeds_dest\n");
    reset_all();

    CHECK(rb_write_pattern(1500) == 1500);
    const uint16_t avail = rb_avail(&s_rb, nullptr);
    CHECK(avail == 1500);

    const uint16_t cap = 512;
    uint8_t *dest      = malloc(cap);
    memset(dest, 0xAA, cap);

    uint16_t got = rb_peek(&s_rb, 0, dest, cap, nullptr);

    CHECK_MSG(got == cap, "应拷满 dest_cap=%u，实际 %u", cap, got);

    int mismatch = -1;
    for (uint16_t i = 0; i < cap; i++)
        if (dest[i] != s_shadow[s_shadow_rd + i]) {
            mismatch = i;
            break;
        }
    CHECK_MSG(mismatch < 0, "第 %d 字节内容不符", mismatch);

    /* 窥视不移动读指针 */
    CHECK(rb_avail(&s_rb, nullptr) == avail);

    free(dest);
}

/** @brief dest_cap 大于可窥视量时返回实际可读量，不多拷 */
static void case_capped_returns_avail_when_dest_larger(void)
{
    printf("case_capped_returns_avail_when_dest_larger\n");
    reset_all();

    CHECK(rb_write_pattern(300) == 300);

    const uint16_t cap = 1024;
    uint8_t *dest      = malloc(cap);
    uint16_t got       = rb_peek(&s_rb, 0, dest, cap, nullptr);

    CHECK_MSG(got == 300, "应返回 avail=300，实际 %u", got);
    free(dest);
}

/** @brief dest_cap == 0 时不得触碰 dest（dest 可为 NULL） */
static void case_capped_zero_cap_touches_nothing(void)
{
    printf("case_capped_zero_cap_touches_nothing\n");
    reset_all();
    CHECK(rb_write_pattern(100) == 100);

    uint16_t got = rb_peek(&s_rb, 0, nullptr, 0, nullptr);
    CHECK_MSG(got == 0, "dest_cap=0 应返回 0，实际 %u", got);
    CHECK(rb_avail(&s_rb, nullptr) == 100);
}

/**
 * @brief 环绕情形：数据跨越缓冲区边界
 *
 * 构造：写满（ring 保留一空槽，实际可写 size-1）→ 读掉接近一整圈 →
 * 再写一段，使数据同时占据尾部与头部两段。跨边界拷贝分两次 memcpy，
 * 是这类实现最容易写错的地方。
 */
static void case_capped_wraparound(void)
{
    printf("case_capped_wraparound\n");
    reset_all();

    uint16_t space = rb_space(&s_rb, nullptr);
    CHECK(rb_write_pattern(space) == space);

    CHECK(rb_read_pattern(1900) == 1900);
    CHECK(rb_write_pattern(200) == 200);

    const uint16_t avail = rb_avail(&s_rb, nullptr);
    CHECK_MSG(avail == 347, "构造结果 avail=%u 应为 347", avail);

    check_contents_match_shadow("环绕");

    /* 确认数据确实跨了边界：读指针 + avail 超过了缓冲区容量 */
    CHECK((uint32_t)s_rb.read_index + avail > TEST_RB_SIZE);

    /* 跨边界处按 cap 截断也要正确 */
    uint8_t *dest = malloc(avail);
    uint16_t got  = rb_peek(&s_rb, 0, dest, (uint16_t)(avail - 1), nullptr);
    CHECK(got == avail - 1);
    free(dest);
}

/** @brief offset 超出可读范围时返回 0 */
static void case_capped_offset_beyond_avail(void)
{
    printf("case_capped_offset_beyond_avail\n");
    reset_all();
    CHECK(rb_write_pattern(10) == 10);

    uint8_t dest[64];
    CHECK(rb_peek(&s_rb, 10, dest, sizeof(dest), nullptr) == 0);
    CHECK(rb_peek(&s_rb, 999, dest, sizeof(dest), nullptr) == 0);
}

/* ================================================================
 *  rb_peek 的单接口契约（这次收敛要守住的点）
 *
 *  收敛前有两套窥视 API：旧 `rb_peek` 的 `len` 是"想要多少"，只按缓冲区自身容量
 *  夹紧、不知道 dest 有多大；另一版收 `dest_cap` 作硬上限（安全版）。收敛后安全版
 *  成为唯一的 `rb_peek`，旧语义不再存在。下面两个用例只钉住**这次收敛**的契约点，
 *  已被其它用例覆盖到的就不再重复：
 *    - dest 比可读量小 → 返回 dest_cap、不越界：见 case_capped_never_exceeds_dest。
 *    - offset 越界或 offset >= avail → 0：见 case_capped_offset_beyond_avail。
 * ================================================================ */

/**
 * @brief 核心不变量：小数组 + dest_cap 超过可读量时，只写数组容量那么多
 *
 * 这是这次收敛要守住的核心不变量。故正好取旧契约会越界写的那副形状：
 * 缓冲区里堆到 2047 字节（远多于 dest 的 512），第 4 个参数给的是 `dest_cap`
 * （= sizeof(dest)），而不是"想要 2047 字节"。旧契约会照 2047 全写进这 512 字节
 * 的栈数组；现在返回值被夹到小数组的容量，数组内容也只被写这么多——ASan 红区下无越界。
 */
static void case_peek_dest_cap_is_hard_limit(void)
{
    printf("case_peek_dest_cap_is_hard_limit\n");
    reset_all();

    /* 塞到可读上限 2047（ring 保留一空槽），远多于 dest 的 512 */
    CHECK(rb_write_pattern(2047) == 2047);
    const uint16_t avail = rb_avail(&s_rb, nullptr);
    CHECK(avail == 2047);

    uint8_t dest[512];
    memset(dest, 0xAA, sizeof(dest));

    uint16_t got = rb_peek(&s_rb, 0, dest, sizeof(dest), nullptr);

    CHECK_MSG(got == sizeof(dest), "应夹到 dest_cap=%u，实际 %u", (unsigned)sizeof(dest), got);

    int mismatch = -1;
    for (uint16_t i = 0; i < (uint16_t)sizeof(dest); i++)
        if (dest[i] != s_shadow[s_shadow_rd + i]) {
            mismatch = i;
            break;
        }
    CHECK_MSG(mismatch < 0, "第 %d 字节内容不符", mismatch);

    /* 窥视不移动读指针 */
    CHECK(rb_avail(&s_rb, nullptr) == avail);
}

/**
 * @brief dest 够大时按可读量返回：是 avail-offset，不是 dest_cap、也不是缓冲区容量
 *
 * 收敛后返回值只由"可读量"与 dest_cap 两者夹紧，与 rb 自身容量无关——
 * 若实现改回按缓冲区容量返回，本用例在 dest_cap 大于可读量时就会拿到错值。
 */
static void case_peek_returns_avail_minus_offset(void)
{
    printf("case_peek_returns_avail_minus_offset\n");
    reset_all();

    CHECK(rb_write_pattern(1000) == 1000);

    uint8_t dest[1024]; /* 容得下，故 dest_cap 不是限制项 */

    /* offset=300 → 可读量 700；dest_cap=1024 大于它，不该被当成返回值 */
    const uint16_t got = rb_peek(&s_rb, 300, dest, sizeof(dest), nullptr);
    CHECK_MSG(got == 700, "应返回 avail-offset=700，实际 %u", got);

    int mismatch = -1;
    for (uint16_t i = 0; i < 700; i++)
        if (dest[i] != s_shadow[s_shadow_rd + 300 + i]) {
            mismatch = i;
            break;
        }
    CHECK_MSG(mismatch < 0, "第 %d 字节内容不符", mismatch);
}

/* ================================================================
 *  入口
 * ================================================================ */

int main(void)
{
    printf("=== test_ring_buffer ===\n");

    case_capped_never_exceeds_dest();
    case_capped_returns_avail_when_dest_larger();
    case_capped_zero_cap_touches_nothing();
    case_capped_wraparound();
    case_capped_offset_beyond_avail();
    case_peek_dest_cap_is_hard_limit();
    case_peek_returns_avail_minus_offset();

    if (g_failures) {
        printf("=== %d 项断言失败 ===\n", g_failures);
        return 1;
    }
    printf("=== 全部通过 ===\n");
    return 0;
}
