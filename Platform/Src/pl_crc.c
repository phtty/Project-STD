/**
 * @file        pl_crc.c
 * @brief       CRC 硬件抽象（hw_device_initcall 优先级 3）
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
hw_device_initcall(pl_crc_init);

pl_crc_handle_t pl_crc_get_handle(void)
{
    return (pl_crc_handle_t)&hcrc;
}

uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len)
{
    (void)h;
    size_t word_cnt = (len + 3) / 4;

    /* 已 4 字节对齐的数据直接透传，避免拷贝 */
    if (((uintptr_t)data & 3) == 0 && (len & 3) == 0) {
        return HAL_CRC_Calculate(&hcrc, (uint32_t *)data, word_cnt);
    }

    /* 非对齐输入：拷贝到临时 buffer 对齐后计算 */
    uint32_t buf[64];
    if (word_cnt > 64) word_cnt = 64;
    memset(buf, 0, word_cnt * 4);
    memcpy(buf, data, len);
    return HAL_CRC_Calculate(&hcrc, buf, word_cnt);
}
