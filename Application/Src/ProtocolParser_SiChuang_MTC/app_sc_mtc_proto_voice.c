/**
 * @file    app_sc_mtc_proto_voice.c
 * @brief   四川 MTC 费显协议（1E）固定语音播报
 *
 * '7' 命令 '0'~'7' 固定语音通过 dev_rs232_voice 直送 TTS 板（USART6 旁路），
 * 参照青海 _voice.c 用法。动态变量（车型/金额/总重/超重）TTS 板不支持变量合成，
 * 本实现按文档语义取核心用语播报，并在注释标注取舍。
 */

#include "app_sc_mtc_proto_voice.h"

#include <stdint.h>
#include <string.h>

#include "dev_rs232_voice.h"
#include "text_cvt.h"

/* '7' 命令 '0'~'7' 固定语音（UTF-8 字面量，播报前运行时转 GBK 送语音板） */
static const char *const s_mtc_fixed_voices[8] = {
    /* '0' 您好，请交费，谢谢合作，祝您一路平安（文档含车型/金额变量，不拼读） */
    "您好，请交费，谢谢合作，祝您一路平安",
    /* '1' 您好，请交费（文档含总重/超重/金额变量，不拼读） */
    "您好，请交费",
    /* '2' 您好（文档含车型变量，不拼读） */
    "您好",
    /* '3' 谢谢合作，祝您一路平安（文档含总重/超重/金额变量，不拼读） */
    "谢谢合作，祝您一路平安",
    /* '4' 谢谢合作，祝您一路平安 */
    "谢谢合作，祝您一路平安",
    /* '5' 月票车，请通行 */
    "月票车，请通行",
    /* '6' 免费车，请通行 */
    "免费车，请通行",
    /* '7' 车辆闯关 */
    "车辆闯关",
};

/**
 * @brief  播报固定语音（'7' 命令 '0'~'7'）。
 * @param  idx  语音索引（0~7）。
 */
void sc_mtc_voice_play_fixed(uint8_t idx)
{
    if (idx >= (sizeof(s_mtc_fixed_voices) / sizeof(s_mtc_fixed_voices[0]))) {
        return;
    }

    /* 语音板要求 GBK：UTF-8 源文运行时转换后播报 */
    uint8_t gbk[40];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(s_mtc_fixed_voices[idx], (uint32_t)strlen(s_mtc_fixed_voices[idx]),
              (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}