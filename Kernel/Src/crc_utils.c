/**
 * @file        crc_utils.c
 * @brief       软件 CRC 算法实现
 */

#include "crc_utils.h"

uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0x8408;
            else
                crc >>= 1;
        }
    }

    return ~crc;
}

uint16_t crc16_ibm(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001; /* 0x8005 反射 */
            else
                crc >>= 1;
        }
    }

    return crc;
}
