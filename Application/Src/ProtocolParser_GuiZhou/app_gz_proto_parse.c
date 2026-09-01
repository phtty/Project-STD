/**
 * @file    app_gz_proto_parse.c
 * @brief   贵州常规费显协议帧解析（纯解析：不发包、不渲染、不碰外设）
 *
 * 帧格式：'{' + 命令字 + 二进制长度 + 参数 + '}'。
 * 协议原文：协议文档 06 贵州常规费显协议（2020-06-17 修订）。
 * 长度边界与截断等按设备实测行为对齐，差异点见各 case 注释。
 */

#include "app_gz_proto_parse.h"

/**
 * @brief  将贵州协议命令码字节转换为内部命令枚举。
 * @param  c  协议中的命令码字节（ASCII '1'~'9','A','B' 或二进制 0x01/0x02）。
 * @return 对应的协议命令枚举；非法字节返回 GZ_PCMD_INVALID。
 */
static gz_pcmd_t gz_char_to_cmd(uint8_t c)
{
    switch (c) {
        case '1':  return GZ_PCMD_HOST_QUERY;
        case '2':  return GZ_PCMD_SELF_CHECK;
        case '3':  return GZ_PCMD_ONE_LINE;
        case '4':  return GZ_PCMD_FULL_SCREEN;
        case '5':  return GZ_PCMD_CLEAR;
        case '6':  return GZ_PCMD_FIXED_FORMAT;
        case '7':  return GZ_PCMD_CIVIL_VOICE;
        case '8':  return GZ_PCMD_BRIGHTNESS;
        case '9':  return GZ_PCMD_VOLUME;
        case 'A':  return GZ_PCMD_PERIPHERAL;
        case 'B':  return GZ_PCMD_FEE_VOICE;
        case 0x01: return GZ_PCMD_FILL_ALL;
        case 0x02: return GZ_PCMD_VERSION;
        default:   return GZ_PCMD_INVALID;
    }
}

uint32_t gz_amount_to_fen(const uint8_t *str, uint16_t len)
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
 * @brief  解析贵州协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 * @note   长度字段为二进制字节值，表示参数区长度。
 *         '4' 命令文本中的回车对 0x0A 0x0D 原样透传：0x0A 由渲染引擎换行，
 *         0x0D 非 GBK 前导字节由渲染引擎跳过，无需归一化。
 *         '6' 命令字段拆分不在 parse 层（不原地改写帧数据），由 cmd 层完成。
 */
gz_parsed_cmd_t gz_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    gz_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < 4 || raw[0] != '{' || raw[raw_len - 1] != '}') {
        cmd.sta = GZ_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.cmd = gz_char_to_cmd(raw[1]);
    if (cmd.cmd == GZ_PCMD_INVALID) {
        cmd.sta = GZ_PARSE_ERR_CMD;
        return cmd;
    }

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t declared_len = raw[2];
    if ((uint16_t)(raw_len - 4) < declared_len) {
        cmd.sta = GZ_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.data     = &raw[3];
    cmd.data_len = declared_len;
    cmd.sta      = GZ_PARSE_OK;

    switch (cmd.cmd) {
        case GZ_PCMD_HOST_QUERY: /* '1' 主机查询：无参数 */
            if (declared_len != 0)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_SELF_CHECK: /* '2' 自检：无参数 */
            if (declared_len != 0)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_ONE_LINE: /* '3' 单行：颜色 + 行号 + 文本 */
            /* 长度边界 3~18 按设备实测行为（len≤2 或 len≥19 丢弃） */
            if (declared_len < 3 || declared_len > 18) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.one_line.color    = (uint8_t)(cmd.data[0] - '0');
            cmd.p.one_line.row      = (uint8_t)(cmd.data[1] - '1');
            cmd.p.one_line.text     = &cmd.data[2];
            cmd.p.one_line.text_len = declared_len - 2;
            if (cmd.p.one_line.color > 2 || cmd.p.one_line.row > 4)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_FULL_SCREEN: /* '4' 全屏可编辑：颜色 + X + Y + 文本 */
            /* 长度边界 4~86 按设备实测行为（len≤3 或 len≥87 丢弃） */
            if (declared_len < 4 || declared_len > 86) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.full_screen.color    = (uint8_t)(cmd.data[0] - '0');
            cmd.p.full_screen.x        = cmd.data[1];
            cmd.p.full_screen.y        = cmd.data[2];
            cmd.p.full_screen.text     = &cmd.data[3];
            cmd.p.full_screen.text_len = declared_len - 3;
            /* len>71 时文本截断为 64 字节渲染（按设备实测行为） */
            if (declared_len > 71)
                cmd.p.full_screen.text_len = 64;
            if (cmd.p.full_screen.color > 2)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_CLEAR: /* '5' 全屏清除：无参数 */
            if (declared_len != 0) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            break;
        case GZ_PCMD_FIXED_FORMAT: /* '6' 固定格式：颜色 + 客货 + '|' 分隔 5 字段 */
            /* 长度边界 3~90 按设备实测行为 */
            if (declared_len < 3 || declared_len > 90) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fixed.color   = (uint8_t)(cmd.data[0] - '0');
            cmd.p.fixed.type    = (uint8_t)(cmd.data[1] - '0');
            cmd.p.fixed.raw     = &cmd.data[2];
            cmd.p.fixed.raw_len = declared_len - 2;
            if (cmd.p.fixed.color > 2 || cmd.p.fixed.type > 1)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_CIVIL_VOICE: /* '7' 礼貌用语语音：索引 '0'~'3' */
            if (declared_len != 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.civil.idx = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.civil.idx > 3)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_BRIGHTNESS: /* '8' 亮度：'0'~'5'，0=自动调节 */
            if (declared_len != 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.brightness.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.brightness.level > 5)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_VOLUME: /* '9' 音量：'1'~'5' */
            if (declared_len != 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.volume.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.volume.level < 1 || cmd.p.volume.level > 5)
                cmd.sta = GZ_PARSE_ERR_PARAM;
            break;
        case GZ_PCMD_PERIPHERAL: /* 'A' 外设：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警 */
            if (declared_len != 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.peripheral.ctrl = cmd.data[0];
            break;
        case GZ_PCMD_FEE_VOICE: /* 'B' 费额语音：金额 ASCII 串（整数/小数） */
            if (declared_len < 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fee.amount_fen = gz_amount_to_fen(cmd.data, declared_len);
            break;
        case GZ_PCMD_FILL_ALL: /* 0x01 全屏点亮：参数 1B 二进制 01红/02绿/03黄 */
            /* 03=黄色按协议文档原文；设备实测行为为蓝色，不沿袭 */
            if (declared_len != 1 || cmd.data[0] < 1 || cmd.data[0] > 3) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fill_all.color = cmd.data[0] - 1; /* 归一化 0 红 / 1 绿 / 2 黄 */
            break;
        case GZ_PCMD_VERSION: /* 0x02 获取版本号：参数 1B（协议示例 00） */
            if (declared_len != 1) {
                cmd.sta = GZ_PARSE_ERR_PARAM;
                break;
            }
            break;
        default:
            break;
    }

    return cmd;
}