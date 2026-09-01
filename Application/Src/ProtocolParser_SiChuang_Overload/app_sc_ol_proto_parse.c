/**
 * @file    app_sc_ol_proto_parse.c
 * @brief   四川治超屏协议（1F，3.5.1 串口方式）帧解析
 *
 * 帧结构：FF + 长度(含头尾总长，07~FF，0xFE 排除) + 命令 + 亮度(00~FF) + 数据 + BCC + FF。
 * BCC = 帧头/长度/命令/亮度/数据段五字段逐字节异或（不含尾部 FF）。
 * 全屏显示 80 数据段可变长（≤249B，随长度字段）；行显示 81~88 数据段可变长（=总长-6，≤24B）；
 * 清屏/亮度/通行灯/黄闪/查询类帧长 7。行显示 8 行与 80 全屏显示。
 */

#include "app_sc_ol_proto_parse.h"

/**
 * @brief  治超帧 BCC 计算：帧头(含)到数据段(含)逐字节异或（不含尾部 FF）。
 * @param  raw      原始帧。
 * @param  raw_len  帧长度。
 * @return BCC 期望值（位于 raw[raw_len-2]）。
 */
static uint8_t _sc_ol_bcc_calc(const uint8_t *raw, uint16_t raw_len)
{
    uint8_t bcc = 0;
    for (uint16_t i = 0; i + 2U < raw_len; i++) { /* [0, len-3]，不含 BCC(len-2) 与尾 FF(len-1) */
        bcc ^= raw[i];
    }
    return bcc;
}

/**
 * @brief  将治超协议命令字节转换为内部命令枚举。
 * @note   80=全屏显示，81~88=第 1~8 行。
 * @param  c  命令字节。
 * @return 对应的协议命令枚举；非法返回 SC_OL_PCMD_INVALID。
 */
static sc_ol_pcmd_t _sc_ol_char_to_cmd(uint8_t c)
{
    switch (c) {
        case 0x80: return SC_OL_PCMD_FULL_SCREEN;
        case 0x81: return SC_OL_PCMD_LINE_1;
        case 0x82: return SC_OL_PCMD_LINE_2;
        case 0x83: return SC_OL_PCMD_LINE_3;
        case 0x84: return SC_OL_PCMD_LINE_4;
        case 0x85: return SC_OL_PCMD_LINE_5;
        case 0x86: return SC_OL_PCMD_LINE_6;
        case 0x87: return SC_OL_PCMD_LINE_7;
        case 0x88: return SC_OL_PCMD_LINE_8;
        case 0x94: return SC_OL_PCMD_CLEAR;
        case 0x96: return SC_OL_PCMD_BRIGHTNESS;
        case 0x99: return SC_OL_PCMD_LANE_LIGHT;
        case 0x98: return SC_OL_PCMD_YELLOW_FLASH;
        case 0xA0: return SC_OL_PCMD_QUERY_CONTENT;
        case 0xB6: return SC_OL_PCMD_QUERY_BRIGHT;
        case 0xB9: return SC_OL_PCMD_QUERY_LANE;
        case 0xB8: return SC_OL_PCMD_QUERY_FLASH;
        default:   return SC_OL_PCMD_INVALID;
    }
}

/**
 * @brief  解析治超协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sc_ol_parsed_cmd_t sc_ol_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    sc_ol_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < SC_OL_FRAME_LEN_MIN || raw_len > SC_OL_FRAME_LEN_MAX ||
        raw[0] != 0xFF || raw[raw_len - 1U] != 0xFF) {
        cmd.sta = SC_OL_PARSE_ERR_FRAME;
        return cmd;
    }

    /* 长度字段必须等于帧总长 */
    if (raw[1] != raw_len) {
        cmd.sta = SC_OL_PARSE_ERR_FRAME;
        return cmd;
    }

    /* BCC 校验：帧头/长度/命令/亮度/数据段五字段异或 */
    if (_sc_ol_bcc_calc(raw, raw_len) != raw[raw_len - 2U]) {
        cmd.sta = SC_OL_PARSE_ERR_BCC;
        return cmd;
    }

    cmd.cmd = _sc_ol_char_to_cmd(raw[2]);
    if (cmd.cmd == SC_OL_PCMD_INVALID) {
        cmd.sta = SC_OL_PARSE_ERR_CMD;
        return cmd;
    }

    cmd.data     = &raw[4];
    cmd.data_len = (uint16_t)(raw_len - 6U); /* 去掉 FF+len+cmd+bright+BCC+FF */

    switch (cmd.cmd) {
        case SC_OL_PCMD_FULL_SCREEN:
            /* 全屏显示：数据段可变长（≤249B = 长度字段上限 255 - 6 帧开销），
             * 整屏自动换行渲染 */
            cmd.p.full_screen.text     = &raw[4];
            cmd.p.full_screen.text_len = (uint16_t)(raw_len - 6U);
            cmd.sta = SC_OL_PARSE_OK;
            break;

        case SC_OL_PCMD_LINE_1:
        case SC_OL_PCMD_LINE_2:
        case SC_OL_PCMD_LINE_3:
        case SC_OL_PCMD_LINE_4:
        case SC_OL_PCMD_LINE_5:
        case SC_OL_PCMD_LINE_6:
        case SC_OL_PCMD_LINE_7:
        case SC_OL_PCMD_LINE_8:
            /* 变长数据段（数据长度 = 总长 - 6，无固定 16B；
             * 渲染层 FONT16 上限 24B 截断，不足不补空格） */
            cmd.p.line.row      = (uint8_t)(cmd.cmd - SC_OL_PCMD_LINE_1);
            cmd.p.line.text     = &raw[4];
            cmd.p.line.text_len = (uint16_t)(raw_len - 6U);
            cmd.sta = (cmd.p.line.text_len <= SC_OL_LINE_TEXT_MAX)
                          ? SC_OL_PARSE_OK : SC_OL_PARSE_ERR_PARAM;
            break;

        case SC_OL_PCMD_CLEAR:
            cmd.sta = (raw_len == SC_OL_FRAME_LEN_MIN) ? SC_OL_PARSE_OK : SC_OL_PARSE_ERR_PARAM;
            break;

        case SC_OL_PCMD_BRIGHTNESS:
        case SC_OL_PCMD_LANE_LIGHT:
        case SC_OL_PCMD_YELLOW_FLASH:
            if (raw_len != SC_OL_FRAME_LEN_MIN) {
                cmd.sta = SC_OL_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.byte_val.val = raw[4];
            if ((cmd.cmd == SC_OL_PCMD_LANE_LIGHT || cmd.cmd == SC_OL_PCMD_YELLOW_FLASH) &&
                cmd.p.byte_val.val > 1U) {
                cmd.sta = SC_OL_PARSE_ERR_PARAM; /* 通行灯/黄闪仅 00/01 */
            } else {
                cmd.sta = SC_OL_PARSE_OK;
            }
            break;

        case SC_OL_PCMD_QUERY_CONTENT:
        case SC_OL_PCMD_QUERY_BRIGHT:
        case SC_OL_PCMD_QUERY_LANE:
        case SC_OL_PCMD_QUERY_FLASH:
            cmd.sta = (raw_len == SC_OL_FRAME_LEN_MIN) ? SC_OL_PARSE_OK : SC_OL_PARSE_ERR_PARAM;
            break;

        default:
            cmd.sta = SC_OL_PARSE_ERR_CMD;
            break;
    }

    return cmd;
}