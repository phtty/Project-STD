/**
 * @file    app_cq_proto_parse.h
 * @brief   重庆高速二代费显协议（CQ）帧解析接口
 */

#pragma once

#include "app_cq_proto.h"

/**
 * @brief  解析后的 CQ 命令。
 * @note   text1/voice1 的文本指针指向 parse 层内部静态缓冲（单消费者任务，
 *         下次解析前有效）；调用方须在 parse 返回后立即执行，不得跨帧保存。
 */
typedef struct {
    cq_parse_sta_t sta;
    cq_pcmd_t cmd;
    union {
        struct { /* text1 */
            bool scr_clear; /* scr==0 → 先全屏清黑 */
            cq_line_t ln[8]; /* ln0~ln7 */
        } text1;
        struct { /* tra1 */
            uint8_t turn; /* 0=红叉(r) / 1=绿箭(g) */
        } tra1;
        struct { /* light1 */
            int level; /* 0~15 */
        } light1;
        struct { /* voice_control1 */
            int level; /* 0~15 */
        } voice_ctrl1;
        struct { /* voice1 */
            const char *text; /* GB2312 字节，保留 '|' 组合符 */
            uint16_t text_len;
        } voice1;
        struct { /* warn1 */
            int second; /* 0 关 / >0 倒计时 / -1 常开 */
        } warn1;
        struct { /* screen */
            int led;
        } screen;
        struct { /* full */
            int color; /* 0红/1绿/2黄/3白 */
        } full;
        cq_setip_t setip; /* setip */
    } p;
} cq_parsed_cmd_t;

/**
 * @brief  解析 CQ 原始帧（纯解析：不渲染、不碰外设、不发包）。
 *
 * JSON 帧：cJSON_Parse 建树（内存钩子已在 cq_proto_init 换绑 RTOS 堆），
 * 按 "cmd" 字段提取各命令参数，文本字段拷贝至内部静态缓冲后删树。
 * 二进制帧：12 字节精确匹配重启 / 搜索两帧。
 *
 * @param  raw      原始帧数据。
 * @param  raw_len  帧长度。
 * @return 解析结果；sta 非 CQ_PARSE_OK 时调用方静默丢弃。
 */
cq_parsed_cmd_t cq_parse_frame(const uint8_t *raw, uint16_t raw_len);