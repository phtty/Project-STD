/**
 * @file    app_gz_proto_voice.h
 * @brief   贵州协议语音播报接口
 */
#pragma once

#include <stdint.h>

/**
 * @brief  播报文明用语。
 * @param  idx  文明用语索引（0~3）。
 */
void gz_voice_civil(uint8_t idx);

/**
 * @brief  播报费额提示语。
 * @param  amount_fen  费额，单位分。
 */
void gz_voice_fee_amount(uint32_t amount_fen);