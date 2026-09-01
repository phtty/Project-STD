/**
 * @file    app_gz_proto_voice.c
 * @brief   贵州常规费显协议语音播报
 *
 * 文明用语索引 → UTF-8 字面量 → UTF8ToGBK 运行时转 GBK → dev_rs232_voice_play
 * （语音板 TTS 旁路 USART6）。费额语音按金额（分）构造「您好请交费X元」。
 */

#include "app_gz_proto_voice.h"

#include <stdio.h>
#include <string.h>

#include "dev_rs232_voice.h"
#include "text_cvt.h"

/* 文明用语（UTF-8 字面量，播报前运行时转 GBK 送语音板）。
 * '0' 按设备实测行为用「贵州高速公路」（协议文档原文为「贵州公路」） */
static const char *const s_civil_texts[] = {
    "您好！欢迎行驶贵州高速公路",
    "请出示通行卡",
    "谢谢合作！祝您一路平安",
    "交易不成功，请走人工车道",
};

/**
 * @brief  播报文明用语。
 * @param  idx  文明用语索引（0~3）。
 */
void gz_voice_civil(uint8_t idx)
{
    if (idx >= (sizeof(s_civil_texts) / sizeof(s_civil_texts[0]))) {
        return;
    }

    /* 语音板要求 GBK：UTF-8 源文运行时转换后播报 */
    uint8_t gbk[32];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(s_civil_texts[idx], (uint32_t)strlen(s_civil_texts[idx]),
              (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  播报费额提示语。
 * @param  amount_fen  费额，单位分。
 * @note   金额 ≥0.5 元才播报（0 元不播报）；小数保留两位（如 1.20→「1.20元」），
 *         .00 时只播整数（按设备实测行为）。
 */
void gz_voice_fee_amount(uint32_t amount_fen)
{
    if (amount_fen < 50U) {
        return;
    }

    char text[48];
    int len;
    if (amount_fen % 100U == 0U) {
        len = snprintf(text, sizeof(text), "您好请交费%u元", (unsigned)(amount_fen / 100U));
    } else {
        len = snprintf(text, sizeof(text), "您好请交费%u.%02u元", (unsigned)(amount_fen / 100U),
                       (unsigned)(amount_fen % 100U));
    }
    if (len <= 0 || len >= (int)sizeof(text)) {
        return;
    }

    /* 语音板要求 GBK：UTF-8 文本运行时转换后播报 */
    uint8_t gbk[64];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(text, (uint32_t)len, (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}