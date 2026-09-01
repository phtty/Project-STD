/**
 * @file    app_sc_etc_proto_parse.c
 * @brief   四川 ETC 费显协议（1D）帧解析
 *
 * 帧结构：0x0A + 显示方式 + 行号 + 数据 + 0x0D；数据变长（0x0D 定界，无固定 56B 全屏长度），
 * 单行 ≤24B、全屏 ≤145B（GBK，汉字高位在前）。
 * 数据首字节 0x20=清屏（行号 0 时清全屏）；0x30=初始化（实际行为=软件复位）。
 * 返回码约定：00 正常 / 01 数据超长 / 02 帧头帧尾或命令编号错误。
 */

#include "app_sc_etc_proto_parse.h"

/**
 * @brief  ETC 帧各类型最大数据字节数。
 * @note   单行数据 ≤24B（12 汉字）；
 *         全屏无独立上限，仅受 0x0D 定界扫描约束（扫描索引 >148 拒帧）
 *         → 帧总长 ≤149 → 数据 ≤145B（帧长减 4 字节开销：头 3 + 0D）。
 */
#define SC_ETC_DATA_MAX_FULL   (145U) /* 全屏（0x0D 定界扫描索引 ≤148 → 149-4） */
#define SC_ETC_DATA_MAX_LINE   (24U)  /* 单行（上限 24B） */
#define SC_ETC_DATA_MAX_SCROLL (142U) /* 滚屏数据（协议 §9 帧 0A 01 00 md rt st d0..dn 0D：
                                        * 7 字节帧开销 → 149-7=142，与 0x0D 定界上限一致） */

/**
 * @brief  将协议第二字节（显示方式/灯控/亮度/心跳命令位）转换为内部命令枚举。
 * @param  c  命令字节。
 * @return 对应的协议命令枚举；非法返回 SC_ETC_PCMD_INVALID。
 */
static sc_etc_pcmd_t _sc_etc_char_to_cmd(uint8_t c)
{
    switch (c) {
        case 0x00:
        case 0x01: return SC_ETC_PCMD_DISPLAY;
        case 0x36: return SC_ETC_PCMD_LIGHT_RED;
        case 0x37: return SC_ETC_PCMD_LIGHT_GREEN;
        case 0x38: return SC_ETC_PCMD_LIGHT_YELLOW_ON;
        case 0x39: return SC_ETC_PCMD_LIGHT_YELLOW_OFF;
        case 0x40: return SC_ETC_PCMD_BRIGHTNESS;
        case 0x50: return SC_ETC_PCMD_HEARTBEAT;
        default:   return SC_ETC_PCMD_INVALID;
    }
}

/**
 * @brief  解析 ETC 协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sc_etc_parsed_cmd_t sc_etc_parse_frame(const uint8_t *raw, uint16_t raw_len)
{
    sc_etc_parsed_cmd_t cmd = {0};
    if (!raw || raw_len < 3 || raw[0] != 0x0A || raw[raw_len - 1] != 0x0D) {
        cmd.sta = SC_ETC_PARSE_ERR_FRAME;
        return cmd;
    }

    cmd.cmd = _sc_etc_char_to_cmd(raw[1]);
    if (cmd.cmd == SC_ETC_PCMD_INVALID) {
        cmd.sta = SC_ETC_PARSE_ERR_CMD;
        return cmd;
    }

    cmd.data     = &raw[2];
    cmd.data_len = (uint16_t)(raw_len - 3U); /* 去掉 0A + 命令位 + 0D */

    switch (cmd.cmd) {
        case SC_ETC_PCMD_LIGHT_RED:
        case SC_ETC_PCMD_LIGHT_GREEN:
        case SC_ETC_PCMD_LIGHT_YELLOW_ON:
        case SC_ETC_PCMD_LIGHT_YELLOW_OFF:
        case SC_ETC_PCMD_HEARTBEAT:
            if (raw_len != 3U) {
                cmd.sta = SC_ETC_PARSE_ERR_FRAME;
            } else {
                cmd.sta = SC_ETC_PARSE_OK;
            }
            break;

        case SC_ETC_PCMD_BRIGHTNESS:
            if (raw_len != 5U) {
                cmd.sta = SC_ETC_PARSE_ERR_FRAME;
                break;
            }
            cmd.p.brightness.level = raw[2];
            if (cmd.p.brightness.level > 7U) {
                cmd.sta = SC_ETC_PARSE_ERR_PARAM;
            } else {
                cmd.sta = SC_ETC_PARSE_OK;
            }
            break;

        case SC_ETC_PCMD_DISPLAY: {
            if (raw_len < 4U) { /* 0A + 方式 + 行号 + 0D 至少 4 字节 */
                cmd.sta = SC_ETC_PARSE_ERR_PARAM;
                break;
            }
            cmd.p.display.mode = raw[1];
            cmd.p.display.row  = raw[2];
            if (cmd.p.display.mode == 0x01U) {
                /* 滚屏帧（协议 §9，仅全屏有效）：0A 01 00 md rt st d0..dn 0D —
                 * 行号固定 00，跳过 md/rt/st 三控制字节 */
                if (raw[2] != 0U) {
                    cmd.sta = SC_ETC_PARSE_ERR_PARAM;
                    break;
                }
                if (raw_len < 7U) {
                    cmd.sta = SC_ETC_PARSE_ERR_PARAM;
                    break;
                }
                cmd.p.display.text     = &raw[6];
                cmd.p.display.text_len = (uint16_t)(raw_len - 7U);
            } else {
                if (cmd.p.display.row > 6U) { /* 单行行号 1~6，0=全屏 */
                    cmd.sta = SC_ETC_PARSE_ERR_PARAM;
                    break;
                }
                cmd.p.display.text     = &raw[3];
                cmd.p.display.text_len = (uint16_t)(raw_len - 4U);
            }

            uint16_t max_data = SC_ETC_DATA_MAX_SCROLL;
            if (cmd.p.display.mode == 0x00) {
                max_data = (cmd.p.display.row == 0U) ? SC_ETC_DATA_MAX_FULL : SC_ETC_DATA_MAX_LINE;
            }
            if (cmd.p.display.text_len > max_data) {
                cmd.sta = SC_ETC_PARSE_ERR_TOOLONG; /* 返回码 01：数据超长 */
            } else {
                cmd.sta = SC_ETC_PARSE_OK;
            }
            break;
        }

        default:
            cmd.sta = SC_ETC_PARSE_ERR_CMD;
            break;
    }

    return cmd;
}