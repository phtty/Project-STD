/**
 * @file        pl_crc.c
 * @brief       CRC 硬件抽象（hw_pl_initcall 优先级 3）
 *
 * 封装 STM32F4 硬件 CRC32 单元（多项式 0x4C11DB7）。
 */

#include "pl_crc.h"
#include "crc.h"
#include "initcall.h"
#include <string.h>

void pl_crc_init(void)
{
    MX_CRC_Init();
}
hw_pl_initcall(pl_crc_init);

pl_crc_handle_t pl_crc_get_handle(void)
{
    return (pl_crc_handle_t)&hcrc;
}

uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len)
{
    (void)h;
    size_t word_cnt = len / 4;

    /* 已 4 字节对齐且整 word 的数据直接透传，避免拷贝 */
    if (((uintptr_t)data & 3) == 0 && (len & 3) == 0) {
        return HAL_CRC_Calculate(&hcrc, (uint32_t *)data, word_cnt);
    }

    /* 非对齐/含尾字节输入：逐 word 从源组装后喂硬件 (兼容任意对齐, 无栈大缓冲)。
     * 语义与原实现一致: 尾部不足一 word 时高位补零。
     * (旧实现的 uint32_t buf[64] + memcpy(len) 在 len>256B 时栈越界 → 修复) */
    __HAL_CRC_DR_RESET(&hcrc); /* 复位计算单元, 同 HAL_CRC_Calculate */

    size_t w = 0;
    while (w + 4 <= len) {
        uint32_t word;
        memcpy(&word, data + w, 4);
        hcrc.Instance->DR = word;
        w += 4;
    }
    if (w < len) { /* 尾部不足一 word: 补零 */
        uint32_t word = 0;
        memcpy(&word, data + w, len - w);
        hcrc.Instance->DR = word;
    }
    return hcrc.Instance->DR;
}
