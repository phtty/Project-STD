/**
 * @file    dev_rs232_voice.h
 * @brief   RS232 TTS 语音板封装
 */
#pragma once

#include <stdint.h>

#define DEV_RS232_VOICE_MAX_TEXT (200U)

void dev_rs232_voice_play(const uint8_t *gbk_text, uint16_t len);
void dev_rs232_voice_volume(uint8_t level);
