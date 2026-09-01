/**
 * @file    app_gz_proto.h
 * @brief   贵州常规费显协议公共定义
 *
 * 帧格式：'{'(0x7B) + 命令字(1B) + 参数长度(1B 二进制) + 参数(变长) + '}'(0x7D)，
 * 无校验。命令集：'1' 主机查询、'2' 自检、'3' 点阵单行任意显示、'4' 点阵全屏可编辑、
 * '5' 全屏清除、'6' 固定格式显示、'7' 礼貌用语语音播报、'8' 显示亮度设定、
 * '9' 语音音量设定、'A' 外设控制、'B' 收费金额语音播放；
 * 另有 0x01 全屏点亮 / 0x02 获取版本号两个二进制命令字。
 * 协议原文：协议文档 06 贵州常规费显协议（2020-06-17 修订）。
 *
 * 帧头与青海协议完全同构（'{'+cmd+len+'}'），命令字 '1'~'9','A','B' 全部落入青海
 * probe 命令集：全协议构建下青海 probe 先注册（字母序 qh < gz）先认领，
 * 量产由 EIDE 目录排除纪律保证贵州与青海互斥（doc/05-01 §6 兼容矩阵）。
 * 0x01/0x02 为二进制命令字，不属于青海/山东/四川MTC 的 ASCII 命令集，probe 首命令字
 * 快拒，无冲突。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/**
 * @brief 贵州协议支持的命令类型。
 */
typedef enum {
    GZ_PCMD_HOST_QUERY = 0, /**< '1' 主机查询 */
    GZ_PCMD_SELF_CHECK,     /**< '2' 自检 */
    GZ_PCMD_ONE_LINE,       /**< '3' 点阵单行任意显示 */
    GZ_PCMD_FULL_SCREEN,    /**< '4' 点阵全屏可编辑显示 */
    GZ_PCMD_CLEAR,          /**< '5' 全屏清除显示 */
    GZ_PCMD_FIXED_FORMAT,   /**< '6' 固定格式显示（含费额语音） */
    GZ_PCMD_CIVIL_VOICE,    /**< '7' 礼貌用语语音播报 */
    GZ_PCMD_BRIGHTNESS,     /**< '8' 显示亮度设定 */
    GZ_PCMD_VOLUME,         /**< '9' 语音音量设定 */
    GZ_PCMD_PERIPHERAL,     /**< 'A' 外设控制 */
    GZ_PCMD_FEE_VOICE,      /**< 'B' 收费金额语音播放 */
    GZ_PCMD_FILL_ALL,       /**< 0x01 全屏点亮（红/绿/黄） */
    GZ_PCMD_VERSION,        /**< 0x02 获取版本号 */
    GZ_PCMD_INVALID,
} gz_pcmd_t;

/**
 * @brief 贵州协议帧解析状态。
 */
typedef enum {
    GZ_PARSE_OK = 0,
    GZ_PARSE_ERR_FRAME, /**< 帧头/帧尾/长度字段错误 */
    GZ_PARSE_ERR_CMD,   /**< 命令字不在命令集 */
    GZ_PARSE_ERR_PARAM, /**< 参数非法 */
} gz_parse_sta_t;

/**
 * @brief 单行显示参数（'3' 命令）。
 */
typedef struct {
    uint8_t color;       /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t row;         /**< 0~4，对应协议行号 1~5 */
    const uint8_t *text; /**< 显示数据（GBK/ASCII 混合） */
    uint16_t text_len;   /**< 数据字节数 */
} gz_one_line_t;

/**
 * @brief 全屏可编辑显示参数（'4' 命令）。
 */
typedef struct {
    uint8_t color;       /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t x;           /**< 显示区域 X 坐标，最左端为 0 */
    uint8_t y;           /**< 显示区域 Y 坐标，最上端为 0 */
    const uint8_t *text; /**< 显示数据（GBK/ASCII，支持 0x0A/0x0D 回车） */
    uint16_t text_len;   /**< 数据字节数 */
} gz_full_screen_t;

/**
 * @brief 固定格式显示参数（'6' 命令）。
 * @note  字段拆分（'|' 分隔）在 cmd 层完成，此处仅保留原始数据指针。
 */
typedef struct {
    uint8_t color;      /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t type;       /**< 0 客车 / 1 货车 */
    const uint8_t *raw; /**< x0/x1 之后 '|' 分隔 5 字段的原始数据 */
    uint16_t raw_len;   /**< 原始数据字节数 */
} gz_fixed_t;

/**
 * @brief 亮度设置参数（'8' 命令，0=自动调光，1~5 档）。
 */
typedef struct { uint8_t level; } gz_brightness_t;

/**
 * @brief 音量设置参数（'9' 命令，1~5 档）。
 */
typedef struct { uint8_t level; } gz_volume_t;

/**
 * @brief 外设控制参数（'A' 命令，bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 */
typedef struct { uint8_t ctrl; } gz_peripheral_t;

/**
 * @brief 费额语音参数（'B' 命令，金额单位分）。
 */
typedef struct { uint32_t amount_fen; } gz_fee_t;

/**
 * @brief 礼貌用语播报参数（'7' 命令）。
 */
typedef struct { uint8_t idx; } gz_civil_t;

/**
 * @brief 全屏点亮参数（0x01 命令，归一化 0 红 / 1 绿 / 2 黄）。
 */
typedef struct { uint8_t color; } gz_fill_all_t;

/**
 * @brief 贵州协议解析结果。
 */
typedef struct {
    gz_pcmd_t cmd;
    gz_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        gz_one_line_t one_line;
        gz_full_screen_t full_screen;
        gz_fixed_t fixed;
        gz_brightness_t brightness;
        gz_volume_t volume;
        gz_peripheral_t peripheral;
        gz_fee_t fee;
        gz_civil_t civil;
        gz_fill_all_t fill_all;
    } p;
} gz_parsed_cmd_t;

/**
 * @brief  贵州协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void gz_proto_handle_task(void *argument);

/**
 * @brief  贵州协议初始化入口。
 */
void gz_proto_init(void);