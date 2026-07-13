#include "bcc_utils.h"

uint8_t bcc_calcu(const uint8_t data[], uint16_t len)
{
    uint8_t bcc = 0;

    while (len > 0) {
        bcc ^= data[len - 1];
        len--;
    }

    return bcc;
}
