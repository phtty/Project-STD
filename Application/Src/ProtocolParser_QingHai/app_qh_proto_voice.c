#include "app_qh_proto_voice.h"

#include <stdio.h>
#include <string.h>

#include "dev_rs232_voice.h"

static const struct {
    const uint8_t *text;
    uint16_t len;
} s_civil_texts[] = {
    {(const uint8_t *)"\xC4\xFA\xBA\xC3\xA3\xA1\xBB\xB6\xD3\xAD\xD0\xD0\xCA\xBB\xD6\xB9\xCA\xD9\xB9\xAB\xC2\xB7", 24},
    {(const uint8_t *)"\xC7\xEB\xB3\xF6\xCA\xBE\xCD\xA8\xD0\xD0\xBF\xA8", 12},
    {(const uint8_t *)"\xD0\xBB\xD0\xBB\xBA\xCF\xD7\xF7\xA3\xA1\xD7\xA3\xC4\xFA\xD2\xBB\xC2\xB7\xC6\xBD\xB0\xB2", 22},
    {(const uint8_t *)"\xBD\xBB\xD2\xD7\xB2\xBB\xB3\xC9\xB9\xA6\xA3\xAC\xC7\xEB\xD7\xDF\xC8\xCB\xB9\xA4\xB3\xB5\xB5\xC0", 24},
};

/**
 * @brief  播报文明用语。
 * @param  idx  文明用语索引。
 */
void qh_voice_civil(uint8_t idx)
{
    if (idx >= (sizeof(s_civil_texts) / sizeof(s_civil_texts[0]))) {
        return;
    }
    dev_rs232_voice_play(s_civil_texts[idx].text, s_civil_texts[idx].len);
}

/**
 * @brief  播报费额提示语。
 * @param  amount_fen  费额，单位分。
 */
void qh_voice_fee_amount(uint32_t amount_fen)
{
    if (amount_fen < 50U) {
        return;
    }
    uint32_t yuan = amount_fen / 100U;
    char text[64];
    int len = snprintf(text, sizeof(text), "您好请交费%lu元", (unsigned long)yuan);
    if (len > 0 && len < (int)sizeof(text)) {
        dev_rs232_voice_play((const uint8_t *)text, (uint16_t)len);
    }
}
