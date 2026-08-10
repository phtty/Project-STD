/**
 * @file    app_qh_proto.h
 * @brief   青海高速费显协议公共定义
 *
 * 该头文件提供青海协议的命令枚举、解析结果、参数结构体和入口接口。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/**
 * @brief 青海协议支持的命令类型。
 */
typedef enum {
    QH_PCMD_HOST_QUERY = 0,
    QH_PCMD_SELF_CHECK,
    QH_PCMD_ONE_LINE,
    QH_PCMD_FULL_SCREEN,
    QH_PCMD_CLEAR,
    QH_PCMD_FIXED_DISPLAY,
    QH_PCMD_CIVIL_VOICE,
    QH_PCMD_BRIGHTNESS,
    QH_PCMD_VOLUME,
    QH_PCMD_PERIPHERAL,
    QH_PCMD_VOICE,
    QH_PCMD_INVALID,
} qh_pcmd_t;

/**
 * @brief 青海协议帧解析状态。
 */
typedef enum {
    QH_PARSE_OK = 0,
    QH_PARSE_ERR_FRAME,
    QH_PARSE_ERR_CMD,
    QH_PARSE_ERR_PARAM,
} qh_parse_sta_t;

/**
 * @brief 单行显示参数。
 */
typedef struct {
    uint8_t color;
    uint8_t row;
    const uint8_t *text;
    uint16_t text_len;
} qh_one_line_t;

/**
 * @brief 全屏显示参数。
 */
typedef struct {
    uint8_t color;
    uint8_t x;
    uint8_t y;
    const uint8_t *text;
    uint16_t text_len;
} qh_full_screen_t;

/**
 * @brief 固定格式显示参数。
 */
typedef struct {
    uint8_t type;
    const uint8_t *raw;
    uint16_t raw_len;
} qh_fixed_t;

/**
 * @brief 亮度设置参数。
 */
typedef struct { uint8_t level; } qh_brightness_t;

/**
 * @brief 音量设置参数。
 */
typedef struct { uint8_t level; } qh_volume_t;

/**
 * @brief 外设控制参数。
 */
typedef struct { uint8_t ctrl; } qh_peripheral_t;

/**
 * @brief 文明用语播报参数。
 */
typedef struct { uint8_t idx; } qh_civil_t;

/**
 * @brief 费额播报参数。
 */
typedef struct { uint8_t type; uint32_t amount_fen; } qh_fee_t;

/**
 * @brief 青海协议解析结果。
 */
typedef struct {
    qh_pcmd_t cmd;
    qh_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        qh_one_line_t one_line;
        qh_full_screen_t full_screen;
        qh_fixed_t fixed;
        qh_brightness_t brightness;
        qh_volume_t volume;
        qh_peripheral_t peripheral;
        qh_civil_t civil;
        qh_fee_t fee;
    } p;
} qh_parsed_cmd_t;

/**
 * @brief  解析青海协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体。
 */
qh_parsed_cmd_t qh_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  执行青海协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void qh_execute_cmd(channel_t *ch, const qh_parsed_cmd_t *cmd);

/**
 * @brief  青海协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void qh_proto_handle_task(void *argument);

/**
 * @brief  青海协议初始化入口。
 */
void qh_proto_init(void);
