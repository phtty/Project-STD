/**
 * @file    app_qh_proto_voice.h
 * @brief   青海协议语音播报接口
 */
#pragma once

#include <stdint.h>

/**
 * @brief 播报文明用语。
 * @param idx  文明用语索引。
 */
void qh_voice_civil(uint8_t idx);

/**
 * @brief 播报费额提示语。
 * @param amount_fen  费额，单位分。
 */
void qh_voice_fee_amount(uint32_t amount_fen);
