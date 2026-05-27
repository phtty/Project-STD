/**
 * @file    bit_utils.c
 * @brief   芯片无关的位操作（查表实现 trailing zero count）
 */

#include "bit_utils.h"

static const uint8_t ctz_table[32] = {
    0, 1, 28, 2, 29, 14, 24, 3,
    30, 22, 20, 15, 25, 17, 4, 8,
    31, 27, 13, 23, 21, 19, 16, 7,
    26, 12, 18, 6, 11, 5, 10, 9,
};

uint8_t bit_ctz(uint32_t x)
{
    return ctz_table[((x & -x) * 0x077CB531U) >> 27];
}
