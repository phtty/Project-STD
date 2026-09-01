/**
 * @file    app_sd_proto.h
 * @brief   山东车道费额显示器通信协议公共定义
 *
 * 帧格式：'{'(0x7B) + 命令字(1B ASCII) + 参数长度(1B 二进制) + 参数(变长) + '}'(0x7D)，
 * 无 BCC/CRC。命令集：'1' 全屏单色（01红/02绿/03黄）、'2' 取版本号、
 * '3' 单行显示（颜色'0'~'2' + 行号'1'~'5' + GBK 文本）、
 * '4' 全屏可编辑（颜色 + X + Y + 文本，0x0A/0x0D 回车）、'5' 清屏、
 * '7' 亮度（'0'~'5'，0=自动调光）、'8' 外设控制（bit0 绿灯/bit1 红灯/bit2 黄闪报警）。
 * 无 '6' 命令。
 *
 * 帧头与青海协议同构（'{'+cmd+len+'}'），命令字 '1'~'5','7','8' 全部落入青海 probe
 * 命令集：全协议构建下青海 probe 先注册（字母序 qh < sd）先认领，量产由 EIDE
 * 目录排除纪律保证与青海/四川MTC 互斥（doc/05-01 §6 兼容矩阵）。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/**
 * @brief 山东协议支持的命令类型。
 */
typedef enum {
    SD_PCMD_FILL_ALL = 0, /**< '1' 全屏单色显示（01红/02绿/03黄） */
    SD_PCMD_VERSION,      /**< '2' 获取版本号 */
    SD_PCMD_ONE_LINE,     /**< '3' 点阵单行任意显示 */
    SD_PCMD_FULL_SCREEN,  /**< '4' 点阵全屏可编辑显示 */
    SD_PCMD_CLEAR,        /**< '5' 全屏清除显示 */
    SD_PCMD_BRIGHTNESS,   /**< '7' 显示亮度设定 */
    SD_PCMD_PERIPHERAL,   /**< '8' 外设控制（红绿灯/报警） */
    SD_PCMD_INVALID,
} sd_pcmd_t;

/**
 * @brief 山东协议帧解析状态。
 */
typedef enum {
    SD_PARSE_OK = 0,
    SD_PARSE_ERR_FRAME, /**< 帧头/帧尾/长度字段错误 */
    SD_PARSE_ERR_CMD,   /**< 命令字不在命令集 */
    SD_PARSE_ERR_PARAM, /**< 参数非法 */
} sd_parse_sta_t;

/**
 * @brief 单行显示参数（'3' 命令）。
 */
typedef struct {
    uint8_t color;        /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t row;          /**< 0~4，对应协议行号 1~5 */
    const uint8_t *text;  /**< 显示数据（GBK/ASCII 混合） */
    uint16_t text_len;    /**< 数据字节数 */
} sd_one_line_t;

/**
 * @brief 全屏可编辑显示参数（'4' 命令）。
 */
typedef struct {
    uint8_t color;        /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t x;            /**< 显示区域 X 坐标，最左端为 0 */
    uint8_t y;            /**< 显示区域 Y 坐标，最上端为 0 */
    const uint8_t *text;  /**< 显示数据（GBK/ASCII，支持 0x0A/0x0D 回车） */
    uint16_t text_len;    /**< 数据字节数 */
} sd_full_screen_t;

/**
 * @brief 全屏单色显示参数（'1' 命令）。
 */
typedef struct {
    uint8_t color; /**< 归一化 0 红 / 1 绿 / 2 黄（协议参数 01/02/03） */
} sd_fill_all_t;

/**
 * @brief 亮度设置参数（'7' 命令，0=自动调光，1~5 档）。
 */
typedef struct { uint8_t level; } sd_brightness_t;

/**
 * @brief 外设控制参数（'8' 命令，bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 */
typedef struct { uint8_t ctrl; } sd_peripheral_t;

/**
 * @brief 山东协议解析结果。
 */
typedef struct {
    sd_pcmd_t cmd;
    sd_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        sd_one_line_t one_line;
        sd_full_screen_t full_screen;
        sd_fill_all_t fill_all;
        sd_brightness_t brightness;
        sd_peripheral_t peripheral;
    } p;
} sd_parsed_cmd_t;

/**
 * @brief  解析山东协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sd_parsed_cmd_t sd_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  执行山东协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void sd_execute_cmd(channel_t *ch, const sd_parsed_cmd_t *cmd);

/**
 * @brief  山东协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void sd_proto_handle_task(void *argument);

/**
 * @brief  山东协议初始化入口。
 */
void sd_proto_init(void);