/**
 * @file    app_yn_proto_voice.c
 * @brief   云南常规费显协议语音播报
 *
 * 礼貌用语索引 → UTF-8 字面量 → UTF8ToGBK 运行时转 GBK → dev_rs232_voice_play
 * （语音板 TTS 旁路 USART6）。收费金额语音按金额（分）构造「您好请交费X元」：
 * 0 元不播报；小数保留有效小数位（123.4→「123.4」、123.45→「123.45」、123→「123」）。
 */

#include "app_yn_proto_voice.h"

#include <stdio.h>
#include <string.h>

#include "dev_rs232_voice.h"
#include "text_cvt.h"

/* 礼貌用语（UTF-8 字面量，播报前运行时转 GBK 送语音板）。
 * 文本按协议原文：'0' 祝您旅途愉快 / '1' 请出示通行卡 /
 * '2' 谢谢合作、祝您一路平安 / '3' 交易不成功，请走人工车道。 */
static const char *const s_yn_civil_texts[] = {
    "祝您旅途愉快",
    "请出示通行卡",
    "谢谢合作祝您一路平安",
    "交易不成功请走人工车道",
};

/**
 * @brief  播报礼貌用语。
 * @param  idx  礼貌用语索引（0~3）。
 */
void yn_voice_civil(uint8_t idx)
{
    if (idx >= (sizeof(s_yn_civil_texts) / sizeof(s_yn_civil_texts[0]))) {
        return;
    }

    /* 语音板要求 GBK：UTF-8 源文运行时转换后播报 */
    uint8_t gbk[64];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(s_yn_civil_texts[idx], (uint32_t)strlen(s_yn_civil_texts[idx]),
              (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  播报收费金额提示语。
 * @param  amount_fen  收费金额，单位分。
 * @note   协议：0 元不播报；有小数需播报小数。
 *         金额串播报格式：整数 →「您好请交费X元」；
 *         1 位有效小数 →「您好请交费X.Y元」；2 位有效小数 →「您好请交费X.YZ元」
 *         （末位 0 剔除，如 123.40 →「123.4」）。
 */
void yn_voice_fee_amount(uint32_t amount_fen)
{
    if (amount_fen == 0U) {
        return; /* 0 元不播报 */
    }

    char text[48];
    int len;
    if (amount_fen % 100U == 0U) {
        len = snprintf(text, sizeof(text), "您好请交费%u元", (unsigned)(amount_fen / 100U));
    } else if (amount_fen % 10U == 0U) {
        len = snprintf(text, sizeof(text), "您好请交费%u.%u元", (unsigned)(amount_fen / 100U),
                       (unsigned)((amount_fen % 100U) / 10U));
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