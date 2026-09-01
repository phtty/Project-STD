/**
 * @file    app_sd_proto_parse.c
 * @brief   山东车道费额显示器通信协议帧解析（纯解析：不发包、不渲染、不碰外设）
 *
 * 帧格式：'{' + 命令字 + 二进制长度 + 参数 + '}'。
 * 协议原文：39山东车道费额显示器通信协议（命令字 '1'~'5','7','8'，无 '6'）。
 */

#include "app_sd_proto_parse.h"

/**
 * @brief  将山东协议命令码字符转换为内部命令枚举。
 * @param  c  协议中的命令码字符。
 * @return 对应的协议命令枚举；非法字符返回 SD_PCMD_INVALID。
 */
static sd_pcmd_t sd_char_to_cmd(uint8_t c)
{
    switch (c) {
        case '1': return SD_PCMD_FILL_ALL;
        case '2': return SD_PCMD_VERSION;
        case '3': return SD_PCMD_ONE_LINE;
        case '4': return SD_PCMD_FULL_SCREEN;
        case '5': return SD_PCMD_CLEAR;
        case '7': return SD_PCMD_BRIGHTNESS;
        case '8': return SD_PCMD_PERIPHERAL;
        default:  return SD_PCMD_INVALID;
    }
}

/**
 * @brief  解析山东协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 * @note   长度字段为二进制字节值，表示参数区长度。
 *         '4' 命令文本中的回车对 0x0A 0x0D 原样透传：0x0A 由渲染引擎换行，
 *         0x0D 非 GBK 前导字节由渲染引擎跳过，无需归一化。
 */
sd_parsed_cmd_t sd_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    sd_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < 4 || raw[0] != '{' || raw[raw_len - 1] != '}') {
        cmd.sta = SD_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.cmd = sd_char_to_cmd(raw[1]);
    if (cmd.cmd == SD_PCMD_INVALID) {
        cmd.sta = SD_PARSE_ERR_CMD;
        return cmd;
    }

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t declared_len = raw[2];
    if ((uint16_t)(raw_len - 4) < declared_len) {
        cmd.sta = SD_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.data = &raw[3];
    cmd.data_len = declared_len;
    cmd.sta = SD_PARSE_OK;

    switch (cmd.cmd) {
        case SD_PCMD_FILL_ALL: /* '1' 全屏单色：参数 1B 二进制 01红/02绿/03黄（协议原文） */
            if (declared_len != 1 || cmd.data[0] < 1 || cmd.data[0] > 3) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.fill_all.color = cmd.data[0] - 1; /* 归一化 0 红 / 1 绿 / 2 黄 */
            break;
        case SD_PCMD_VERSION: /* '2' 取版本号：参数 1B（协议示例 00） */
            if (declared_len != 1) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            break;
        case SD_PCMD_ONE_LINE: /* '3' 单行：颜色 + 行号 + 文本 */
            if (declared_len < 2) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.one_line.color = (uint8_t)(cmd.data[0] - '0');
            cmd.p.one_line.row = (uint8_t)(cmd.data[1] - '1');
            cmd.p.one_line.text = &cmd.data[2];
            cmd.p.one_line.text_len = declared_len - 2;
            if (cmd.p.one_line.color > 2 || cmd.p.one_line.row > 4)
                cmd.sta = SD_PARSE_ERR_PARAM;
            break;
        case SD_PCMD_FULL_SCREEN: /* '4' 全屏可编辑：颜色 + X + Y + 文本 */
            if (declared_len < 3) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.full_screen.color = (uint8_t)(cmd.data[0] - '0');
            cmd.p.full_screen.x = cmd.data[1];
            cmd.p.full_screen.y = cmd.data[2];
            cmd.p.full_screen.text = &cmd.data[3];
            cmd.p.full_screen.text_len = declared_len - 3;
            if (cmd.p.full_screen.color > 2)
                cmd.sta = SD_PARSE_ERR_PARAM;
            break;
        case SD_PCMD_CLEAR: /* '5' 全屏清除：无参数 */
            if (declared_len != 0) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            break;
        case SD_PCMD_BRIGHTNESS: /* '7' 亮度：'0'~'5'，0=自动调节 */
            if (declared_len != 1) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.brightness.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.brightness.level > 5)
                cmd.sta = SD_PARSE_ERR_PARAM;
            break;
        case SD_PCMD_PERIPHERAL: /* '8' 外设：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警 */
            if (declared_len != 1) {
                cmd.sta = SD_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.peripheral.ctrl = cmd.data[0];
            break;
        default:
            break;
    }

    return cmd;
}