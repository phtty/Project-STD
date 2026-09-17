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
 * 本文件**逐行对齐 Platform/Src/pl_crc.c 的实际行为**，包括它的缺陷：
 * 非对齐分支里 `memcpy(buf, data, len)` 只夹紧了 word_cnt（≤64 字 = 256 字节的
 * 栈缓冲），**没有一起夹紧 len** —— len > 256 时该 memcpy 会写穿栈缓冲。
 * 桩若比真实现"更正确"，测试就会掩盖真实缺陷，所以这里保留该行为，
 * 由 ASan 在触发时给出与实机同形态的报告。
 */

#include "pl_crc.h"

#include <stdint.h>
#include <string.h>

#define CRC32_POLY      0x04C11DB7U
#define CRC32_INIT      0xFFFFFFFFU
#define STUB_WORD_LIMIT 64U /* 与真实现一致：非对齐分支最多 64 字 */

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= (uint32_t)byte << 24;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80000000U) ? ((crc << 1) ^ CRC32_POLY) : (crc << 1);
    return crc;
}

/** @brief 等价的 HAL_CRC_Calculate：按字喂入，字内字节反序（大端） */
static uint32_t crc32_words(const uint8_t *data, size_t word_cnt)
{
    uint32_t crc = CRC32_INIT;
    for (size_t i = 0; i < word_cnt; i++) {
        crc = crc32_byte(crc, data[i * 4 + 3]);
        crc = crc32_byte(crc, data[i * 4 + 2]);
        crc = crc32_byte(crc, data[i * 4 + 1]);
        crc = crc32_byte(crc, data[i * 4 + 0]);
    }
    return crc;
}

pl_crc_handle_t pl_crc_get_handle(void)
{
    return (pl_crc_handle_t)&crc32_byte; /* 桩不需要真句柄，给个非空值 */
}

uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len)
{
    (void)h;
    size_t word_cnt = (len + 3) / 4;

    /* 对齐路径：4 字节对齐且长度为 4 的倍数时零拷贝直通 */
    if (((uintptr_t)data & 3U) == 0U && (len & 3U) == 0U)
        return crc32_words(data, word_cnt);

    /* 非对齐路径：与真实现同样**只**夹紧 word_cnt，len 原样传给 memcpy */
    if (word_cnt > STUB_WORD_LIMIT) word_cnt = STUB_WORD_LIMIT;

    uint32_t buf[STUB_WORD_LIMIT];
    memset(buf, 0, word_cnt * 4);
    memcpy(buf, data, len);
    return crc32_words((const uint8_t *)buf, word_cnt);
}
