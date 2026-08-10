#include "app_qh_proto_parse.h"

/**
 * @brief  将青海协议命令码字符转换为内部命令枚举。
 * @param  c  协议中的命令码字符。
 * @return 对应的协议命令枚举；非法字符返回 QH_PCMD_INVALID。
 */
static qh_pcmd_t qh_char_to_cmd(uint8_t c)
{
    switch (c) {
        case '1': return QH_PCMD_HOST_QUERY;
        case '2': return QH_PCMD_SELF_CHECK;
        case '3': return QH_PCMD_ONE_LINE;
        case '4': return QH_PCMD_FULL_SCREEN;
        case '5': return QH_PCMD_CLEAR;
        case '6': return QH_PCMD_FIXED_DISPLAY;
        case '7': return QH_PCMD_CIVIL_VOICE;
        case '8': return QH_PCMD_BRIGHTNESS;
        case '9': return QH_PCMD_VOLUME;
        case 'A': return QH_PCMD_PERIPHERAL;
        case 'B': return QH_PCMD_VOICE;
        default:  return QH_PCMD_INVALID;
    }
}

/**
 * @brief  解析青海协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 * @note   长度字段为二进制字节值，表示数据区长度。
 */
qh_parsed_cmd_t qh_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    qh_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < 4 || raw[0] != '{' || raw[raw_len - 1] != '}') {
        cmd.sta = QH_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.cmd = qh_char_to_cmd(raw[1]);
    if (cmd.cmd == QH_PCMD_INVALID) {
        cmd.sta = QH_PARSE_ERR_CMD;
        return cmd;
    }

    /* 长度字段是二进制字节值，不是 ASCII 数字。 */
    uint16_t declared_len = raw[2];
    if ((uint16_t)(raw_len - 4) < declared_len) {
        cmd.sta = QH_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.data = &raw[3];
    cmd.data_len = declared_len;
    cmd.sta = QH_PARSE_OK;

    switch (cmd.cmd) {
        case QH_PCMD_ONE_LINE:
            if (declared_len < 2) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.one_line.color = (uint8_t)(cmd.data[0] - '0');
            cmd.p.one_line.row = (uint8_t)(cmd.data[1] - '1');
            cmd.p.one_line.text = &cmd.data[2];
            cmd.p.one_line.text_len = declared_len - 2;
            if (cmd.p.one_line.color > 2 || cmd.p.one_line.row > 4) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_FULL_SCREEN:
            if (declared_len < 3) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.full_screen.color = (uint8_t)(cmd.data[0] - '0');
            cmd.p.full_screen.x = cmd.data[1];
            cmd.p.full_screen.y = cmd.data[2];
            cmd.p.full_screen.text = &cmd.data[3];
            cmd.p.full_screen.text_len = declared_len - 3;
            if (cmd.p.full_screen.color > 2) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_BRIGHTNESS:
            if (declared_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.brightness.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.brightness.level > 5) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_VOLUME:
            if (declared_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.volume.level = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.volume.level < 1 || cmd.p.volume.level > 5) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_PERIPHERAL:
            if (declared_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.peripheral.ctrl = cmd.data[0];
            break;
        case QH_PCMD_CIVIL_VOICE:
            if (declared_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.civil.idx = (uint8_t)(cmd.data[0] - '0');
            if (cmd.p.civil.idx > 3) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_FIXED_DISPLAY:
            if (declared_len < 2) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.fixed.type = (uint8_t)(cmd.data[0] - '0');
            cmd.p.fixed.raw = &cmd.data[1];
            cmd.p.fixed.raw_len = declared_len - 1;
            if (cmd.p.fixed.type > 1) cmd.sta = QH_PARSE_ERR_PARAM;
            break;
        case QH_PCMD_VOICE:
            if (declared_len < 6) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
            cmd.p.fee.type = (uint8_t)(cmd.data[0] - '0');
            cmd.p.fee.amount_fen = 0;
            for (int i = 1; i <= 5; i++) {
                if (cmd.data[i] < '0' || cmd.data[i] > '9') { cmd.sta = QH_PARSE_ERR_PARAM; break; }
                cmd.p.fee.amount_fen = cmd.p.fee.amount_fen * 10U + (uint32_t)(cmd.data[i] - '0');
            }
            break;
        default:
            break;
    }

    return cmd;
}
