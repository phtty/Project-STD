/**
 * @file    app_sc_mtc_proto_voice.h
 * @brief   四川 MTC 费显协议语音播报接口
 */
#pragma once

#include <stdint.h>

/**
 * @brief  播报固定语音（'7' 命令 '0'~'7'）。
 * @param  idx  语音索引（0~7）。
 * @note   静态用语直接送 TTS；车型/金额/总重等动态变量不做拼读
 *         （TTS 板不支持变量合成，后续可扩展数字拼接）。
 */
void sc_mtc_voice_play_fixed(uint8_t idx);