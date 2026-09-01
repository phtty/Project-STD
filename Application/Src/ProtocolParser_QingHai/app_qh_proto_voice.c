#include "app_qh_proto_voice.h"

#include <stdio.h>
#include <string.h>

#include "dev_rs232_voice.h"
#include "text_cvt.h"

/* 文明用语（UTF-8 字面量，播报前运行时转 GBK 送语音板）。
 * '0' 按 doc/04 B.7.6 对齐裸机用「贵州高速公路」（协议文档原文为「高速公路」） */
static const char *const s_civil_texts[] = {
    "您好！欢迎行驶贵州高速公路",
    "请出示通行卡",
    "谢谢合作！祝您一路平安",
    "交易不成功，请走人工车道",
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
        /* 语音板要求 GBK：UTF-8 文本运行时转换后播报 */
        uint8_t gbk[64];
        uint32_t gbk_len = sizeof(gbk);
        UTF8ToGBK(text, (uint32_t)len, (char *)gbk, &gbk_len);
        dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
    }
}
