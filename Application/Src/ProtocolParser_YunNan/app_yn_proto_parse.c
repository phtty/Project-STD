/**
 * @file    app_yn_proto_parse.c
 * @brief   云南常规费显协议帧解析（纯解析：不发包、不渲染、不碰外设）
 *
 * 帧格式：'{' + 命令字 + 二进制长度 + 参数 + '}'。
 * 协议原文：01 云南常规费显协议-云南LED费显P5（2022.7.5）。
 * 参数边界推导：协议定义 24 点阵、一行 12 ASCII / 6 汉字（=12 字节），
 * 本设备 24 点阵一行可容纳 16 ASCII / 8 汉字（=16 字节），'3' 单行文本
 * 上限按 2+16=18 字节（与贵州 3~18 实测边界一致）；非法参数置错不崩溃。
 */

#include "app_yn_proto_parse.h"

/**
 * @brief  将云南协议命令码字节转换为内部命令枚举。
 * @param  c  协议中的命令码字节（ASCII '1'~'9','A','B' 或二进制 0x01/0x02）。
 * @return 对应的协议命令枚举；非法字节返回 YN_PCMD_INVALID。
 */
static yn_pcmd_t yn_char_to_cmd(uint8_t c)
{
    switch (c) {
        case '1':  return YN_PCMD_HOST_QUERY;
        case '2':  return YN_PCMD_SELF_CHECK;
        case '3':  return YN_PCMD_ONE_LINE;
        case '4':  return YN_PCMD_FULL_SCREEN;
        case '5':  return YN_PCMD_CLEAR;
        case '6':  return YN_PCMD_CLEAR_ROW;
        case '7':  return YN_PCMD_CIVIL_VOICE;
        case '8':  return YN_PCMD_BRIGHTNESS;
        case '9':  return YN_PCMD_VOLUME;
        case 'A':  return YN_PCMD_PERIPHERAL;
        case 'B':  return YN_PCMD_FEE_VOICE;
        case 0x01: return YN_PCMD_FILL_ALL;
        case 0x02: return YN_PCMD_VERSION;
        default:   return YN_PCMD_INVALID;
    }
}

uint32_t yn_amount_to_fen(const uint8_t *str, uint16_t len)
{
    uint32_t fen = 0U;
    uint16_t i   = 0U;
    if (str == NULL || len == 0U) {
        return 0U;
    }

    /* 整数部分：前缀数字累加，遇非数字停止（strtod 前缀语义） */
    while (i < len && str[i] >= '0' && str[i] <= '9') {
        fen = fen * 10U + (uint32_t)(str[i] - '0');
        i++;
    }
    fen *= 100U;

    /* 小数部分：最多取两位（%.2f 语义：一位补零，第三位起截断） */
    if (i < len && str[i] == '.') {
        i++;
        uint32_t dec    = 0U;
        uint16_t digits = 0U;
        while (i < len && digits < 2U && str[i] >= '0' && str[i] <= '9') {
            dec = dec * 10U + (uint32_t)(str[i] - '0');
            digits++;
            i++;
        }
        if (digits == 1U) {
            dec *= 10U;
        }
        fen += dec;
    }
    return fen;
}

/**
 * @brief  解析云南协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 * @note   长度字段为二进制字节值，表示参数区长度。
 *         '4' 命令文本中的回车对 0x0A 0x0D / 0x0D 0x0A 原样透传：0x0A 由渲染
 *         引擎换行，0x0D 非 GBK 前导字节由渲染引擎跳过，无需归一化。
 */
yn_parsed_cmd_t yn_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    yn_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < 4 || raw[0] != '{' || raw[raw_len - 1] != '}') {
        cmd.sta = YN_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.cmd = yn_char_to_cmd(raw[1]);
    if (cmd.cmd == YN_PCMD_INVALID) {
        cmd.sta = YN_PARSE_ERR_CMD;
        return cmd;
    }

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t declared_len = raw[2];
    if ((uint16_t)(raw_len - 4) < declared_len) {
        cmd.sta = YN_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.data     = &raw[3];
    cmd.data_len = declared_len;
    cmd.sta      = YN_PARSE_OK;

    switch (cmd.cmd) {
        case YN_PCMD_HOST_QUERY: /* '1' 主机查询：无参数 */
            if (declared_len != 0)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_SELF_CHECK: /* '2' 自检：无参数 */
            if (declared_len != 0)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_ONE_LINE: /* '3' 单行：颜色 + 行号 + 文本 */
            /* 协议一行 12 ASCII / 6 汉字（12 字节）；本设备 24 点阵一行
             * 16 ASCII / 8 汉字（16 字节）→ 文本上限 16 字节，帧参数
             * 长度边界 3~18（与贵州 3~18 边界一致）。
             * 行号范围以协议为准 '1'~'5' 全接受（→0~4，用户决定 2）：
             * 行 5 在 96px 屏高（4 行 FONT_24）下物理不显示，但解析
             * 不拒绝——渲染调用照常发出，越界由渲染层自然丢弃。 */
            if (declared_len < 3 || declared_len > 18) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.one_line.color    = (uint8_t)(cmd.data[0] - '0');
            cmd.p.one_line.row      = (uint8_t)(cmd.data[1] - '1');
            cmd.p.one_line.text     = &cmd.data[2];
            cmd.p.one_line.text_len = declared_len - 2;
            if (cmd.p.one_line.color > 2 || cmd.p.one_line.row > 4)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_FULL_SCREEN: /* '4' 全屏可编辑：颜色 + X + Y + 文本 */
            /* 参数 3 字节固定头 + 文本；上限 86 对齐贵州（len>71 截断 64 为
             * 贵州设备实测行为，云南无实测，不截断——超屏由渲染引擎按边界裁剪）。 */
            if (declared_len < 4 || declared_len > 86) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.full_screen.color    = (uint8_t)(cmd.data[0] - '0');
            cmd.p.full_screen.x        = cmd.data[1];
            cmd.p.full_screen.y        = cmd.data[2];
            cmd.p.full_screen.text     = &cmd.data[3];
            cmd.p.full_screen.text_len = declared_len - 3;
            if (cmd.p.full_screen.color > 2)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_CLEAR: /* '5' 全屏清除：无参数 */
            if (declared_len != 0) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            break;
        case YN_PCMD_CLEAR_ROW: /* '6' 单行清除：行号 '1'~'5'（行 5 同 '3'：
                                 * 协议范围全接受，越界由渲染层自然处理） */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.clear_row.row = (uint8_t)(cmd.data[0] - '1');
            if (cmd.p.clear_row.row > 4)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_CIVIL_VOICE: /* '7' 礼貌用语语音：索引 '0'~'3'（其他预留） */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.civil.idx = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.civil.idx > 3)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_BRIGHTNESS: /* '8' 亮度（用户决定 5，2026-08-24 修订）：
                                  * 参数 1B——0x00（NUL）= 自动亮度，
                                  * ASCII '1'~'8'（0x31~0x38）= 手动档 1~8（8 最亮）。
                                  * 帧长度定界不受 0x00 影响：probe 按二进制 len
                                  * 字段定界 + 尾字节 '}' 校验，本处按 declared_len
                                  * 取参数，0x00 不会被当作字符串终结符。 */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            if (cmd.data[0] == 0x00) {
                cmd.p.brightness.level = 0; /* 0 = 自动亮度 */
            } else if (cmd.data[0] >= '1' && cmd.data[0] <= '8') {
                cmd.p.brightness.level = (uint8_t)(cmd.data[0] - '0'); /* 1~8 */
            } else {
                cmd.sta = YN_PARSE_ERR_PARAM;
            }
            break;
        case YN_PCMD_VOLUME: /* '9' 音量：'1'~'5' */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.volume.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.volume.level < 1 || cmd.p.volume.level > 5)
                cmd.sta = YN_PARSE_ERR_PARAM;
            break;
        case YN_PCMD_PERIPHERAL: /* 'A' 外设：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警 */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.peripheral.ctrl = cmd.data[0];
            break;
        case YN_PCMD_FEE_VOICE: /* 'B' 收费金额语音：金额 ASCII 串（整数/小数） */
            if (declared_len < 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fee.amount_fen = yn_amount_to_fen(cmd.data, declared_len);
            break;
        case YN_PCMD_FILL_ALL: /* 0x01 全屏点亮：参数 1B 二进制（用户决定 10 扩展）：
                                * 01红/02绿/03黄（协议原文三色，保留兼容）/
                                * 04蓝/05紫/06青/07白（P5 户外全彩屏支持 8 色，
                                * 黑色除外——全屏黑等效 '5' 全屏清除）。
                                * DATA0 与 display_color_t 枚举值恒等，
                                * 执行层直接按枚举值渲染。 */
            if (declared_len != 1 || cmd.data[0] < 1 || cmd.data[0] > 7) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fill_all.color = cmd.data[0]; /* 0x01~0x07 → COLOR_RED~COLOR_WHITE */
            break;
        case YN_PCMD_VERSION: /* 0x02 获取版本号：参数 1B（协议示例 00） */
            if (declared_len != 1) {
                cmd.sta = YN_PARSE_ERR_PARAM;
                break;
            }
            break;
        default:
            break;
    }

    return cmd;
}