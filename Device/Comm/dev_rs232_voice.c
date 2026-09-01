/**
 * @file    dev_rs232_voice.c
 * @brief   RS232 TTS 语音板封装
 */

#include "dev_rs232_voice.h"

#include <string.h>

#include "dev_rs232.h"
#include "pl_uart.h"

static pl_uart_handle_t dev_rs232_voice_uart(void)
{
    /* 语音板 TTS 专用 USART6 (PL_UART6)，不再使用 USART3 */
    return pl_uart_get_handle(PL_UART6);
}

static void dev_rs232_voice_send(const uint8_t *frame, uint16_t len)
{
    pl_uart_handle_t uart = dev_rs232_voice_uart();
    if (!uart || !frame || len == 0U) {
        return;
    }

    (void)pl_uart_send(uart, frame, len, 100U);
}

void dev_rs232_voice_play(const uint8_t *gbk_text, uint16_t len)
{
    if (!gbk_text || len == 0U || len > DEV_RS232_VOICE_MAX_TEXT) {
        return;
    }

    uint8_t frame[DEV_RS232_VOICE_MAX_TEXT + 5U];
    const uint16_t payload_len = (uint16_t)(len + 2U);
    if (payload_len > 0xFFU) {
        return;
    }

    frame[0] = 0xFDU;
    frame[1] = 0x00U;
    frame[2] = (uint8_t)payload_len;
    frame[3] = 0x01U;
    frame[4] = 0x01U;
    memcpy(&frame[5], gbk_text, len);

    dev_rs232_voice_send(frame, (uint16_t)(len + 5U));
}

void dev_rs232_voice_volume(uint8_t level)
{
    uint8_t frame[6];
    frame[0] = 0xFDU;
    frame[1] = 0x00U;
    frame[2] = 0x03U;
    frame[3] = 0x01U;
    frame[4] = 0x02U;
    frame[5] = level;
    dev_rs232_voice_send(frame, sizeof(frame));
}
