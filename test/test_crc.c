/**
 * @file    test_crc.c
 * @brief   硬件 CRC32 封装：任意长度、任意对齐都要算全，且不得比原来少算一个字节
 *
 * **为什么需要这个测试**：`pl_crc32_calc` 原先对非 4 字节对齐的输入，是把数据拷进一个
 * `uint32_t buf[64]` 的**栈缓冲**再喂硬件 —— 于是**超过 256 字节的部分被静默截断**。
 *
 * 这个截断极难发现：发收两侧用的是同一个函数、算出来还一致，所以协议"工作正常"，
 * 只是**四分之三的载荷根本没被校验**。而 1KB 级的非对齐输入是完全正常的用法 ——
 * 级联协议的帧头 11 字节，CRC 覆盖的区段从偏移 2 开始，而暂存区本身 4 字节对齐，
 * +2 就不对齐了，必走这条路。
 *
 * 所以本用例钉住的是：**任意长度、任意对齐下，结果都必须与"把全部字节喂进去"一致**。
 * 参照实现是独立的（按大端逐字节推进的 CRC-32/MPEG-2），不是把被测算法的写法抄一遍。
 *
 * 被测代码是生产源码本体（`Platform/Src/pl_crc.c`），不替换、不改写；
 * HAL 的 CRC 桩按真单元语义实现（逐字推进、字内大端）。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pl_crc.h"
#include "stm32f4xx_hal.h"

/* ================================================================
 *  硬件 CRC 单元的桩 —— 按 stm32f4xx_hal_crc.c 的真实语义
 *
 *  配置（Core/Src/crc.c 只设了 Instance，其余全零 → HAL 的默认使能位为 0）：
 *  多项式 0x04C11DB7、初值 0xFFFFFFFF、输入输出均不反转，
 *  InputDataFormat = WORDS —— 即把 32 位字直接写进 DR，单元按 MSB 优先逐位处理。
 *  等价于把每个 4 字节组按**大端序**喂给 CRC-32/MPEG-2。
 * ================================================================ */

#define CRC32_POLY 0x04C11DB7U
#define CRC32_INIT 0xFFFFFFFFU

static uint32_t s_dr; /* 单元当前值 */

static uint32_t _bit(uint32_t crc, uint8_t b)
{
    crc ^= (uint32_t)b << 24;
    for (int i = 0; i < 8; i++) crc = (crc & 0x80000000U) ? ((crc << 1) ^ CRC32_POLY) : (crc << 1);
    return crc;
}

/** @brief 写 DR 一拍：字内按大端逐字节处理 */
static void _feed(uint32_t w)
{
    s_dr = _bit(s_dr, (uint8_t)(w >> 24));
    s_dr = _bit(s_dr, (uint8_t)(w >> 16));
    s_dr = _bit(s_dr, (uint8_t)(w >> 8));
    s_dr = _bit(s_dr, (uint8_t)w);
}

/* 硬件寄存器映射：pl_crc.c 直接写 Instance->DR / 置 CR 的复位位 */
static CRC_TypeDef   s_crc_regs;
CRC_HandleTypeDef    hcrc = {.Instance = &s_crc_regs};
void                 MX_CRC_Init(void) {}

uint32_t HAL_CRC_Calculate(CRC_HandleTypeDef *h, uint32_t pBuffer[], uint32_t BufferLength)
{
    s_dr = CRC32_INIT; /* 真单元：Calculate 会先把 DR 复位到初值 */
    for (uint32_t i = 0; i < BufferLength; i++) _feed(pBuffer[i]);
    h->Instance->DR = s_dr;
    return s_dr;
}

/* 累加：**不**重置，接着当前值继续 —— 真单元就是这个语义，
   pl_crc.c 的非对齐路径靠它把长输入分块算完 */
uint32_t HAL_CRC_Accumulate(CRC_HandleTypeDef *h, uint32_t pBuffer[], uint32_t BufferLength)
{
    for (uint32_t i = 0; i < BufferLength; i++) _feed(pBuffer[i]);
    h->Instance->DR = s_dr;
    return s_dr;
}

/* ---- 独立参照：把整段字节按"每 4 字节一组、组内大端"喂进去 ----
 *
 * 刻意与 pl_crc.c 的写法不同：这里先拼出完整的字节视图再逐字节推进，
 * 被测代码则是逐字 memcpy + 写 DR。 */
static uint32_t reference(const uint8_t *data, size_t len)
{
    uint32_t crc = CRC32_INIT;
    size_t   i   = 0;
    for (; i + 4U <= len; i += 4U) {
        crc = _bit(crc, data[i + 3]);
        crc = _bit(crc, data[i + 2]);
        crc = _bit(crc, data[i + 1]);
        crc = _bit(crc, data[i + 0]);
    }
    if (i < len) { /* 尾巴补零 —— 与"先清零再 memcpy 前 len 字节"同一约定 */
        uint8_t t[4] = {0, 0, 0, 0};
        memcpy(t, data + i, len - i);
        crc = _bit(crc, t[3]);
        crc = _bit(crc, t[2]);
        crc = _bit(crc, t[1]);
        crc = _bit(crc, t[0]);
    }
    return crc;
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

static uint8_t s_buf[4096];

/** @brief 各种长度/对齐下，`pl_crc32_calc` 必须等于"把全部字节喂进去" */
static void case_matches_reference(void)
{
    TEST_BEGIN("任意长度与对齐下都等于独立参照（不多算也不少算）");

    for (uint16_t off = 0; off < 4; off++) {
        for (uint16_t len = 0; len <= 600; len += 7) {
            uint8_t *p = s_buf + off;
            for (uint16_t i = 0; i < len; i++) p[i] = (uint8_t)(i * 31U + off);

            uint32_t got  = pl_crc32_calc(pl_crc_get_handle(), p, len);
            uint32_t want = reference(p, len);

            if (got != want) {
                CHECK_MSG(0, "对齐偏移 %u、长度 %u：得到 %08X，参照 %08X", (unsigned)off,
                          (unsigned)len, (unsigned)got, (unsigned)want);
                return; /* 一条就够，别刷屏 */
            }
        }
    }
    CHECK_MSG(1, "offset×len 全组合");
}

/** @brief 本用例的存在理由：>256 字节的非对齐输入**不得被截断** */
static void case_long_unaligned_not_truncated(void)
{
    TEST_BEGIN("超过 256 字节的非对齐输入不得被截断（原缺陷的回归守卫）");

    /* 起点故意不对齐 —— 级联的 CRC 区段正是 scratch+2 */
    uint8_t *p = s_buf + 2;

    for (uint16_t len = 257; len <= 1044; len += 131) {
        for (uint16_t i = 0; i < len; i++) p[i] = (uint8_t)(i * 7U + 3U);

        uint32_t got  = pl_crc32_calc(pl_crc_get_handle(), p, len);
        uint32_t want = reference(p, len);
        CHECK_MSG(got == want, "长度 %u：得到 %08X，参照 %08X（差 %08X）", (unsigned)len,
                  (unsigned)got, (unsigned)want, (unsigned)(got ^ want));
    }

    /* 更强的一条：改动尾部字节必须改变结果。
       截断的实现只算前 256 字节，这里远在 256 之后的字节动了它却不变。 */
    uint16_t len = 1024;
    for (uint16_t i = 0; i < len; i++) p[i] = (uint8_t)i;
    uint32_t a = pl_crc32_calc(pl_crc_get_handle(), p, len);
    p[len - 1] ^= 0x5A; /* 最后一个字节 */
    p[len - 300] ^= 0x5A; /* 以及 256 之外的某处 */
    uint32_t b = pl_crc32_calc(pl_crc_get_handle(), p, len);
    CHECK_MSG(a != b, "改了 256 字节之后的内容，CRC 却没变 —— 说明那段没参与计算");
}

/** @brief 对齐且整字的快路径（现有 IAP 走路的那条）行为不变 */
static void case_aligned_fast_path(void)
{
    TEST_BEGIN("对齐且整字的输入走零拷贝快路径，结果与参照一致");

    uint32_t *p = (uint32_t *)s_buf; /* s_buf 静态 → 至少 4 字节对齐 */
    for (uint16_t len = 0; len <= 1044; len += 4) {
        uint8_t *q = (uint8_t *)p;
        for (uint16_t i = 0; i < len; i++) q[i] = (uint8_t)(i ^ 0x5A);

        uint32_t got  = pl_crc32_calc(pl_crc_get_handle(), q, len);
        uint32_t want = reference(q, len);
        if (got != want) {
            CHECK_MSG(0, "长度 %u：得到 %08X，参照 %08X", (unsigned)len, (unsigned)got,
                      (unsigned)want);
            return;
        }
    }
    CHECK_MSG(1, "0..1044 整字长度全对");
}

/** @brief 空输入与空指针不得崩 */
static void case_edge(void)
{
    TEST_BEGIN("长度 0 与空指针");

    uint32_t z = pl_crc32_calc(pl_crc_get_handle(), s_buf, 0);
    CHECK_MSG(z == 0xFFFFFFFFU, "长度 0 应返回初值 0xFFFFFFFF，得到 %08X", (unsigned)z);

    uint32_t n = pl_crc32_calc(pl_crc_get_handle(), nullptr, 16);
    CHECK_MSG(n == 0xFFFFFFFFU, "空指针应返回初值，得到 %08X", (unsigned)n);
}

/* ================================================================ */

int main(void)
{
    case_matches_reference();
    case_long_unaligned_not_truncated();
    case_aligned_fast_path();
    case_edge();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
