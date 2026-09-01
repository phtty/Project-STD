/**
 * @file    app_sc_ol_proto.h
 * @brief   四川治超屏协议（1F，3.5.1 串口方式）公共定义
 *
 * 帧格式：`FF + 长度(1B，含头尾总长 07~FF，0xFE 排除防 RLS 吞帧) + 命令 + 亮度(00~FF) +
 * 数据(行显示变长 ≤24B；全屏可变长 ≤249B) + BCC + FF`。
 * BCC = 帧头/长度/命令/亮度/数据段五字段逐字节异或。
 * 全屏显示 80；行显示 81~88（支持 8 行）；清屏 94；亮度 96（00=自动调光）；
 * 通行灯 99（00红/01绿）；黄闪 98（00关/01开）；
 * 查询类（屏须应答）：A0 取显示内容 → A1~A8 每行独立帧；B6 取亮度；B9 取通行灯；B8 取黄闪。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/** 帧最小/最大长度（FF 07 94 00 BCC FF = 7；长度字段 1 字节，上限 255，容纳 0x80 全屏长数据段） */
#define SC_OL_FRAME_LEN_MIN (7U)
#define SC_OL_FRAME_LEN_MAX (0xFFU)

/** 每行数据字节数（8 列 × 2，应答帧用；行显示数据=列数×2）与行文本上限（FONT16 24B，超长截断） */
#define SC_OL_BYTES_PER_LINE (16U)
#define SC_OL_LINE_TEXT_MAX  (24U)
#define SC_OL_LINE_COUNT     (8U)

/**
 * @brief 治超协议命令类型。
 */
typedef enum {
    SC_OL_PCMD_FULL_SCREEN = 0, /**< 80 全屏显示 */
    SC_OL_PCMD_LINE_1,       /**< 81 第一行显示 */
    SC_OL_PCMD_LINE_2,       /**< 82 第二行显示 */
    SC_OL_PCMD_LINE_3,       /**< 83 第三行显示 */
    SC_OL_PCMD_LINE_4,       /**< 84 第四行显示 */
    SC_OL_PCMD_LINE_5,       /**< 85 第五行显示 */
    SC_OL_PCMD_LINE_6,       /**< 86 第六行显示 */
    SC_OL_PCMD_LINE_7,       /**< 87 第七行显示 */
    SC_OL_PCMD_LINE_8,       /**< 88 第八行显示 */
    SC_OL_PCMD_CLEAR,        /**< 94 清屏 */
    SC_OL_PCMD_BRIGHTNESS,   /**< 96 亮度调节 */
    SC_OL_PCMD_LANE_LIGHT,   /**< 99 通行灯（00红/01绿） */
    SC_OL_PCMD_YELLOW_FLASH, /**< 98 黄闪（00关/01开） */
    SC_OL_PCMD_QUERY_CONTENT,/**< A0 获取费显当前显示内容 */
    SC_OL_PCMD_QUERY_BRIGHT, /**< B6 获取费显亮度 */
    SC_OL_PCMD_QUERY_LANE,   /**< B9 获取通行灯状态 */
    SC_OL_PCMD_QUERY_FLASH,  /**< B8 获取黄闪状态 */
    SC_OL_PCMD_INVALID,
} sc_ol_pcmd_t;

/**
 * @brief 治超协议帧解析状态。
 */
typedef enum {
    SC_OL_PARSE_OK = 0,
    SC_OL_PARSE_ERR_FRAME,
    SC_OL_PARSE_ERR_CMD,
    SC_OL_PARSE_ERR_BCC,
    SC_OL_PARSE_ERR_PARAM,
} sc_ol_parse_sta_t;

/**
 * @brief 行显示参数。
 */
typedef struct {
    uint8_t row;          /**< 行索引 0~7 */
    const uint8_t *text;  /**< 行数据（变长，≤24B 截断渲染） */
    uint16_t text_len;
} sc_ol_line_t;

/**
 * @brief 全屏显示参数（数据段可变长 ≤249B，随长度字段 07~FF）。
 */
typedef struct {
    const uint8_t *text;
    uint16_t text_len;
} sc_ol_full_screen_t;

/**
 * @brief 单字节数值参数（亮度/通行灯/黄闪）。
 */
typedef struct {
    uint8_t val;
} sc_ol_byte_val_t;

/**
 * @brief 治超协议解析结果。
 */
typedef struct {
    sc_ol_pcmd_t cmd;
    sc_ol_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        sc_ol_full_screen_t full_screen;
        sc_ol_line_t line;
        sc_ol_byte_val_t byte_val;
    } p;
} sc_ol_parsed_cmd_t;

/**
 * @brief  解析治超协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sc_ol_parsed_cmd_t sc_ol_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  执行治超协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void sc_ol_execute_cmd(channel_t *ch, const sc_ol_parsed_cmd_t *cmd);

/**
 * @brief  治超协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void sc_ol_proto_handle_task(void *argument);

/**
 * @brief  治超协议初始化入口。
 */
void sc_ol_proto_init(void);