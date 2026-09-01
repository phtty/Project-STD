/**
 * @file    app_sc_mtc_proto.h
 * @brief   四川 MTC 费显协议（1E 方案二）公共定义
 *
 * 方案二（LED 点阵）：帧 `'{' + 命令('1'~'9','A') + 参数 + '}'`——'}' 定界变长，
 * 无长度字段、无 BCC（各命令逐字节扫 '}' 定帧，协议扫描上限 228；
 * 本设备 probe 上限 SC_MTC_PAYLOAD_MAX=74B 队列约束）。
 * 带 BCC 变体不加判别：BCC 字节视为内容尾部一并渲染，BCC 不校验。
 * 主机查询 `0A 46 0A` → 应答 `0A 64 0A`；清屏 `0A 46 0D`。
 * 附加 7B 40~45 原始帧族（创迪科技添加协议，同样 '}' 定界）：
 * 40 改波特率 / 41 点阵大小 / 42 字体 / 43 协议类型 / 44 全屏点亮 / 45 取版本号。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/**
 * @brief MTC 协议命令类型。
 */
typedef enum {
    SC_MTC_PCMD_INIT = 0,        /**< '1' 初始化 */
    SC_MTC_PCMD_SELF_CHECK,      /**< '2' 自检 */
    SC_MTC_PCMD_ONE_LINE,        /**< '3' 点阵单行任意显示 */
    SC_MTC_PCMD_FULL_SCREEN,     /**< '4' 点阵全屏任意显示 */
    SC_MTC_PCMD_CLEAR,           /**< '5' 全屏清除 */
    SC_MTC_PCMD_FIXED_DISPLAY,   /**< '6' 固定格式显示 */
    SC_MTC_PCMD_VOICE,           /**< '7' 语音播报 */
    SC_MTC_PCMD_BRIGHTNESS,      /**< '8' 显示亮度设定 */
    SC_MTC_PCMD_VOLUME,          /**< '9' 语音音量设定 */
    SC_MTC_PCMD_COLOR,           /**< 'A' 显示颜色设定 */
    SC_MTC_PCMD_RAW_BAUD,        /**< 7B 40 修改波特率 */
    SC_MTC_PCMD_RAW_DOT_SIZE,    /**< 7B 41 修改点阵大小 */
    SC_MTC_PCMD_RAW_FONT,        /**< 7B 42 修改字体类型 */
    SC_MTC_PCMD_RAW_PROTO,       /**< 7B 43 设置协议类型 */
    SC_MTC_PCMD_RAW_FILL_ALL,    /**< 7B 44 全屏点亮 */
    SC_MTC_PCMD_RAW_VERSION,     /**< 7B 45 获取版本号 */
    SC_MTC_PCMD_HOST_QUERY,      /**< 0A 46 0A 主机查询 */
    SC_MTC_PCMD_HOST_CLEAR,      /**< 0A 46 0D 主机清屏 */
    SC_MTC_PCMD_INVALID,
} sc_mtc_pcmd_t;

/**
 * @brief MTC 协议帧解析状态。
 */
typedef enum {
    SC_MTC_PARSE_OK = 0,
    SC_MTC_PARSE_ERR_FRAME,
    SC_MTC_PARSE_ERR_CMD,
    SC_MTC_PARSE_ERR_BCC,
    SC_MTC_PARSE_ERR_PARAM,
} sc_mtc_parse_sta_t;

/**
 * @brief 单行显示参数。
 */
typedef struct {
    uint8_t row;          /**< 行号 0~5（协议 '1'~'6'） */
    const uint8_t *text;  /**< 变长显示数据（'}' 定界，最长 70B） */
    uint16_t text_len;
} sc_mtc_one_line_t;

/**
 * @brief 全屏显示参数（变长文本，渲染按 16B/行切分）。
 */
typedef struct {
    const uint8_t *text;
    uint16_t text_len;
} sc_mtc_full_screen_t;

/**
 * @brief 固定格式显示参数。
 */
typedef struct {
    uint8_t type;         /**< '0' 客车(12B) / '1' 货车(21B) */
    const uint8_t *raw;
    uint16_t raw_len;
} sc_mtc_fixed_t;

/**
 * @brief 语音播报参数。
 */
typedef struct {
    uint8_t idx;          /**< 固定语音 '0'~'7'；'8' 为自定义文本 */
    const uint8_t *text;  /**< idx='8' 时的 GBK 文本 */
    uint16_t text_len;
} sc_mtc_voice_t;

/**
 * @brief 亮度/音量/颜色等单字节参数。
 */
typedef struct {
    uint8_t val;
} sc_mtc_byte_val_t;

/**
 * @brief 7B 40 修改波特率参数。
 */
typedef struct {
    uint8_t mode;   /**< 00=9600, 01=115200 */
    uint16_t baud16;/**< 波特率低 16 位（0x2580=9600, 0xC200=115200） */
} sc_mtc_baud_t;

/**
 * @brief MTC 协议解析结果。
 */
typedef struct {
    sc_mtc_pcmd_t cmd;
    sc_mtc_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        sc_mtc_one_line_t one_line;
        sc_mtc_full_screen_t full_screen;
        sc_mtc_fixed_t fixed;   /**< 固定格式显示（X1 起 raw_len=11/20 字节） */
        sc_mtc_voice_t voice;
        sc_mtc_byte_val_t byte_val; /* 亮度/音量/颜色/点阵大小/字体/协议类型/全屏点亮 */
        sc_mtc_baud_t baud;
    } p;
} sc_mtc_parsed_cmd_t;

/**
 * @brief  解析 MTC 协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sc_mtc_parsed_cmd_t sc_mtc_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  执行 MTC 协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void sc_mtc_execute_cmd(channel_t *ch, const sc_mtc_parsed_cmd_t *cmd);

/**
 * @brief  MTC 协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void sc_mtc_proto_handle_task(void *argument);

/**
 * @brief  MTC 协议初始化入口。
 */
void sc_mtc_proto_init(void);