/**
 * @file        app_onbon.c
 * @brief       Onbon BX-Y LED 控制卡协议实现（Application 层）
 *
 * 提供 PHY0 层字符转义、PHY1 层封帧，
 * 对外暴露 onbon_send_text() 发送接口。
 */

#include "app_onbon.h"
#include "crc_utils.h"
#include "app_dispatch.h"
#include "pl_uart.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 *  封帧转义
 *
 *  扫描原始 PHY1 数据，将帧头/帧尾/转义前缀替换为双字节序列。
 *  校验和长度计算均基于转义前的原始数据。
 * ================================================================ */

size_t onbon_escape(uint8_t *dst, const uint8_t *src, size_t src_len)
{
    size_t w = 0;
    for (size_t i = 0; i < src_len; i++) {
        uint8_t b = src[i];
        if (b == ONBON_STX) { /* 0xA5 → 0xA6 0x02 */
            dst[w++] = ONBON_STX_ESC;
            dst[w++] = ONBON_ESC_VAL;
        } else if (b == ONBON_STX_ESC) { /* 0xA6 → 0xA6 0x01 */
            dst[w++] = ONBON_STX_ESC;
            dst[w++] = ONBON_ESC_MASK;
        } else if (b == ONBON_ETX) { /* 0x5A → 0x5B 0x02 */
            dst[w++] = ONBON_ETX_ESC;
            dst[w++] = ONBON_ESC_VAL;
        } else if (b == ONBON_ETX_ESC) { /* 0x5B → 0x5B 0x01 */
            dst[w++] = ONBON_ETX_ESC;
            dst[w++] = ONBON_ESC_MASK;
        } else {
            dst[w++] = b;
        }
    }
    return w;
}

/* ================================================================
 *  上下文初始化 — 填入全部默认值
 * ================================================================ */

void onbon_ctx_init(onbon_ctx_t *ctx)
{
    ctx->area_x  = 0x00 | 0x8000;
    ctx->area_y  = 0x00;
    ctx->area_w  = ONBON_SCREEN_WIDTH | 0x8000; /* 192 (像素) */
    ctx->area_h  = ONBON_SCREEN_HEIGHT;         /* 32 (像素) */
    ctx->area_id = 0x01;

    ctx->run_mode     = ONBON_RUN_LOOP;
    ctx->timeout      = 0x02;
    ctx->display_mode = ONBON_DISP_STILL;
    ctx->exit_mode    = 0x00;
    ctx->speed        = 0x01;
    ctx->stay_time    = 0x0a;

    ctx->font      = ONBON_FONT_HEI;
    ctx->font_size = 32;
    ctx->color     = ONBON_COLOR_RED;
    ctx->bg_color  = ONBON_BG_NONE;
    ctx->h_align   = ONBON_ALIGN_H_CENTER;
    ctx->v_align   = ONBON_ALIGN_V_CENTER;
}

/* ================================================================
 *  onbon_send_text — 拼装完整帧并发送
 *
 *  帧结构（由内向外）:
 *    1. 转义前缀 + text         → 用户可见字符串
 *    2. onbon_area_data_t       → 区域头 + data
 *    3. A3 06 payload           → process_mode + delete + area_num + area
 *    4. onbon_req_t             → 协议层请求
 *    5. onbon_header_t + CRC16  → PHY1 层
 *    6. onbon_escape            → 转义
 *    7. 8×0xA5 + escaped + 0x5A → PHY0 帧
 * ================================================================ */

#define TX_BUF_SIZE 2048

void onbon_send_text(const onbon_ctx_t *ctx, const char *text)
{
    // channel_t *ch = app_channel_get(CH_ID_RS485);
    // if (ch == NULL || ch->state != CH_STATE_UP)
    //     return;

    static uint8_t raw[TX_BUF_SIZE];     /* PHY1 原始数据 */
    static uint8_t escaped[TX_BUF_SIZE]; /* 转义后数据 */
    uint8_t *wr = raw;                   /* 写指针 */

    /* ---- 1. 填充 onbon_header_t (14B) ---- */
    onbon_header_t *header = (onbon_header_t *)wr;
    header->dst_addr[0]    = ONBON_DST_ADDR & 0xFF;
    header->dst_addr[1]    = (ONBON_DST_ADDR >> 8) & 0xFF;
    header->src_addr[0]    = ONBON_SRC_ADDR & 0xFF;
    header->src_addr[1]    = (ONBON_SRC_ADDR >> 8) & 0xFF;
    header->reserved[0]    = 0x00;
    header->reserved[1]    = 0x00;
    header->reserved[2]    = 0x00;
    header->opt            = 0x00;
    header->check_mode     = 0x00;
    header->display_mode   = 0x00;
    header->device_type    = ONBON_DEVICE_TYPE;
    header->proto_ver      = ONBON_PROTO_VER;
    /* data_len 最后回填 */
    wr += sizeof(onbon_header_t);

    /* ---- 2. 填充 A3 06 请求头 (5B) ---- */
    onbon_req_t *req = (onbon_req_t *)wr;
    req->cmd_group   = 0xA3;
    req->cmd         = 0x06;
    req->response    = 0x02; /* 不必回复 */
    req->process     = 0x01;
    req->reserved    = 0x00;
    wr += sizeof(onbon_req_t);

    /* ---- 3. A3 06 payload: ProcessMode(1) + Reserved(1) + DeleteAreaNum(1) + AreaNum(1) ---- */
    *wr++ = 0x00; /* DeleteAreaNum: 删除区域个数 */
    *wr++ = 0x01; /* AreaNum: 区域编号 */

    /* ---- 4. 构造转义前缀 + text ---- */
    char fmt_buf[32];
    int fmt_len = snprintf(fmt_buf, sizeof(fmt_buf), "\\F%d%03d\\C%d\\B%d", ctx->font, ctx->font_size, ctx->color, ctx->bg_color);
    // int fmt_len       = snprintf(fmt_buf, sizeof(fmt_buf), "\\F%d%03d\\C%d", ctx->font, ctx->font_size, ctx->color);
    uint16_t text_len = (uint16_t)strlen(text);
    uint16_t data_len = (uint16_t)(fmt_len + text_len);

    /* ---- 5. AreaDataLen (2B 小端) ---- */
    uint16_t area_total = (uint16_t)(sizeof(onbon_area_data_t) + data_len);
    *wr++               = area_total & 0xFF;
    *wr++               = (area_total >> 8) & 0xFF;

    /* ---- 6. 填充 onbon_area_data_t (27B) ---- */
    onbon_area_data_t *area = (onbon_area_data_t *)wr;
    area->area_type         = 0x00;
    area->area_x[0]         = ctx->area_x & 0xFF;
    area->area_x[1]         = ((ctx->area_x >> 8) & 0xFF) | 0x80;
    area->area_y[0]         = ctx->area_y & 0xFF;
    area->area_y[1]         = (ctx->area_y >> 8) & 0xFF;
    area->area_w[0]         = ctx->area_w & 0xFF;
    area->area_w[1]         = ((ctx->area_w >> 8) & 0xFF) | 0x80;
    area->area_h[0]         = ctx->area_h & 0xFF;
    area->area_h[1]         = (ctx->area_h >> 8) & 0xFF;
    area->area_id           = ctx->area_id;
    area->line_spacing      = 0;
    area->run_mode          = ctx->run_mode;
    area->timeout[0]        = ctx->timeout & 0xFF;
    area->timeout[1]        = (ctx->timeout >> 8) & 0xFF;
    area->sound_mode        = 0;
    area->ext_para_len      = 0;
    area->text_align        = (ctx->v_align << 2) | ctx->h_align;
    area->single_line       = 0x02;
    area->new_line          = 0x02;
    area->display_mode      = ctx->display_mode;
    area->exit_mode         = ctx->exit_mode;
    area->speed             = ctx->speed;
    area->stay_time         = ctx->stay_time;
    area->data_len[0]       = data_len & 0xFF;
    area->data_len[1]       = (data_len >> 8) & 0xFF;
    area->data_len[2]       = 0;
    area->data_len[3]       = 0;
    wr += sizeof(onbon_area_data_t);

    /* ---- 7. 拷贝转义前缀 + text 到 area.data[] ---- */
    memcpy(wr, fmt_buf, fmt_len);
    wr += fmt_len;
    memcpy(wr, text, text_len);
    wr += text_len;

    /* ---- 8. 回填 header.data_len（数据域长度 = w - header - 包头） ---- */
    uint16_t payload_len = (uint16_t)(wr - raw - sizeof(onbon_header_t));
    header->data_len[0]  = payload_len & 0xFF;
    header->data_len[1]  = (payload_len >> 8) & 0xFF;

    /* ---- 9. CRC16 (header + data) 追加到 PHY1 末尾 ---- */
    uint16_t crc = crc16_ibm(raw, (size_t)(wr - raw));
    *wr++        = crc & 0xFF;
    *wr++        = (crc >> 8) & 0xFF;

    /* ---- 10. 转义 ---- */
    size_t pkt_len = (size_t)(wr - raw);
    size_t esc_len = onbon_escape(escaped, raw, pkt_len);

    /* ---- 11. 组 PHY0 帧:  8×0xA5 + escaped + 0x5A, 直接写入 raw ---- */
    static uint8_t frame[TX_BUF_SIZE] = {0};
    memset(frame, 0, sizeof(frame));
    uint8_t *f = frame;

    for (int i = 0; i < 8; i++)
        *f++ = ONBON_STX;
    memcpy(f, escaped, esc_len);
    f += esc_len;
    *f++ = ONBON_ETX;

    // channel_send(ch, frame, (uint16_t)(f - frame));
    // pl_uart_set_rs485_tx(true);
    pl_uart_send(pl_uart_get_handle(PL_UART1), frame, (uint16_t)(f - frame), 50);
    // pl_uart_set_rs485_tx(false);
}
