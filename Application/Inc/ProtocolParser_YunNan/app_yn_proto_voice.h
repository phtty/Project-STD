/**
 * @file    app_yn_proto_voice.h
 * @brief   云南协议语音播报接口
 */
#pragma once

#include <stdint.h>

/**
 * @brief  播报礼貌用语。
 * @param  idx  礼貌用语索引（0~3）。
 */
void yn_voice_civil(uint8_t idx);

/**
 * @brief  播报收费金额提示语（0 元不播报，小数播小数）。
 * @param  amount_fen  收费金额，单位分。
 */
void yn_voice_fee_amount(uint32_t amount_fen);