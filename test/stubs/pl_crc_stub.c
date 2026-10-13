/**
 * @file    pl_crc_stub.c
 * @brief   pl_crc 的 host 实现 —— 复刻 STM32 硬件 CRC 单元的语义
 *
 * 配置来源：Core/Src/crc.c 的 MX_CRC_Init 只设置了 hcrc.Instance，其余字段全零；
 * 而 HAL 里 DEFAULT_POLYNOMIAL_ENABLE 与 DEFAULT_INIT_VALUE_ENABLE 恰好都是 0，
 * 所以实际生效的是"全默认"配置：
 *     多项式 0x04C11DB7，初值 0xFFFFFFFF，输入/输出均不反转，
 *     InputDataFormat = CRC_INPUTDATA_FORMAT_WORDS
 *
 * WORDS 格式下 HAL 把 32 位字直接写进 DR、单元按 MSB 优先逐位处理，因此等价于
 * 把每个 4 字节组按**大端序**喂给 CRC-32/MPEG-2。
 *
 * **本文件与 Platform/Src/pl_crc.c 行为一致**（含非对齐/非整字输入的尾部补零约定）。
 *
 * 早先本文件刻意保留过真实现的一个缺陷（非对齐分支只夹 `word_cnt` 不夹 `len`，
 * 于是 len > 256 会写穿栈缓冲）。那个缺陷真实现已经修掉，本文件随之改为正确实现 ——
 * "桩比真实现更正确会掩盖缺陷"这条原则依然成立，只是现在两者都对了。
 * 真实现本身的正确性由 `test/test_crc.c` 链接**真文件**来测，不靠桩。
 */

#include "pl_crc.h"

#include <stdint.h>
#include <string.h>

#define CRC32_POLY 0x04C11DB7U
#define CRC32_INIT 0xFFFFFFFFU

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= (uint32_t)byte << 24;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80000000U) ? ((crc << 1) ^ CRC32_POLY) : (crc << 1);
    return crc;
}

/** @brief 一个 32 位字的硬件行为：字内按大端逐字节处理 */
static uint32_t crc32_word(uint32_t crc, const uint8_t *w)
{
    crc = crc32_byte(crc, w[3]);
    crc = crc32_byte(crc, w[2]);
    crc = crc32_byte(crc, w[1]);
    crc = crc32_byte(crc, w[0]);
    return crc;
}

pl_crc_handle_t pl_crc_get_handle(void)
{
    return (pl_crc_handle_t)&crc32_byte; /* 桩不需要真句柄，给个非空值 */
}

uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len)
{
    (void)h;
    if (!data) return CRC32_INIT;

    /* 对齐且整字：零拷贝直通 */
    if (((uintptr_t)data & 3U) == 0U && (len & 3U) == 0U) {
        uint32_t crc = CRC32_INIT;
        for (size_t i = 0; i < len; i += 4)
            crc = crc32_word(crc, data + i);
        return crc;
    }

    /* 逐字喂入，**无长度上限**；尾巴不足一个字时高位补零 */
    uint32_t crc = CRC32_INIT;
    size_t   i   = 0;
    for (; i + 4U <= len; i += 4U)
        crc = crc32_word(crc, data + i);

    if (i < len) {
        uint8_t tail[4] = {0, 0, 0, 0};
        memcpy(tail, data + i, len - i);
        crc = crc32_word(crc, tail);
    }
    return crc;
}
