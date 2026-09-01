/**
 * @file    app_yn_proto.h
 * @brief   云南常规费显协议公共定义
 *
 * 帧格式：'{'(0x7B) + 命令字(1B) + 参数长度(1B 二进制) + 参数(变长) + '}'(0x7D)，
 * 无校验。与青海/山东/贵州帧结构完全同构。
 * 命令集：'1' 主机查询、'2' 自检、'3' 点阵单行任意显示、'4' 点阵全屏可编辑显示、
 * '5' 全屏清除、'6' 单行清除、'7' 礼貌用语语音播报、'8' 显示亮度设定、
 * '9' 语音音量设定、'A' 外设控制、'B' 收费金额语音播放；
 * 另有 0x01 全屏点亮 / 0x02 获取版本号两个二进制命令字。
 * 协议原文：01 云南常规费显协议-云南LED费显P5（2022.7.5，云南弥玉项目 2022-S134）。
 *
 * 帧头与青海协议同构（'{'+cmd+len+'}'），命令字 '1'~'9','A','B' 全部落入青海
 * probe 命令集：全协议构建下青海 probe 先注册（源码收录序 qh 在前）先认领，
 * 量产由 EIDE 目录排除纪律保证云南与其余 '{' 帧族互斥（doc/05-01 §6 兼容矩阵）。
 * 0x01/0x02 为二进制命令字，不属于青海/山东/四川MTC 的 ASCII 命令集，probe 首命令字
 * 快拒，无冲突。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/* 0x02 版本号应答：回固件 PROGRAM_CODE（app_boot.h 编译期常量），
 * 不再使用硬编码 "YN_FX_P5_1.0"（用户决定 7，2026-08-24 修订）。 */

/**
 * @brief 云南协议支持的命令类型。
 */
typedef enum {
    YN_PCMD_HOST_QUERY = 0, /**< '1' 主机查询 */
    YN_PCMD_SELF_CHECK,     /**< '2' 自检 */
    YN_PCMD_ONE_LINE,       /**< '3' 点阵单行任意显示 */
    YN_PCMD_FULL_SCREEN,    /**< '4' 点阵全屏可编辑显示 */
    YN_PCMD_CLEAR,          /**< '5' 全屏清除显示 */
    YN_PCMD_CLEAR_ROW,      /**< '6' 单行清除显示 */
    YN_PCMD_CIVIL_VOICE,    /**< '7' 礼貌用语语音播报 */
    YN_PCMD_BRIGHTNESS,     /**< '8' 显示亮度设定 */
    YN_PCMD_VOLUME,         /**< '9' 语音音量设定 */
    YN_PCMD_PERIPHERAL,     /**< 'A' 外设控制 */
    YN_PCMD_FEE_VOICE,      /**< 'B' 收费金额语音播放 */
    YN_PCMD_FILL_ALL,       /**< 0x01 全屏点亮（01红~07白 七色，用户决定 10） */
    YN_PCMD_VERSION,        /**< 0x02 获取版本号 */
    YN_PCMD_INVALID,
} yn_pcmd_t;

/**
 * @brief 云南协议帧解析状态。
 */
typedef enum {
    YN_PARSE_OK = 0,
    YN_PARSE_ERR_FRAME, /**< 帧头/帧尾/长度字段错误 */
    YN_PARSE_ERR_CMD,   /**< 命令字不在命令集 */
    YN_PARSE_ERR_PARAM, /**< 参数非法 */
} yn_parse_sta_t;

/**
 * @brief 单行显示参数（'3' 命令）。
 */
typedef struct {
    uint8_t color;       /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t row;         /**< 0~4，对应协议行号 1~5 */
    const uint8_t *text; /**< 显示数据（GBK/ASCII 混合） */
    uint16_t text_len;   /**< 数据字节数 */
} yn_one_line_t;

/**
 * @brief 全屏可编辑显示参数（'4' 命令）。
 */
typedef struct {
    uint8_t color;       /**< 0 红 / 1 绿 / 2 黄 */
    uint8_t x;           /**< 显示区域 X 坐标，最左端为 0 */
    uint8_t y;           /**< 显示区域 Y 坐标，最上端为 0 */
    const uint8_t *text; /**< 显示数据（GBK/ASCII，支持 0x0A 0x0D / 0x0D 0x0A 回车换行） */
    uint16_t text_len;   /**< 数据字节数 */
} yn_full_screen_t;

/**
 * @brief 单行清除参数（'6' 命令，行号 '1'~'5'）。
 */
typedef struct { uint8_t row; } yn_clear_row_t;

/**
 * @brief 亮度设置参数（'8' 命令）。
 * @note  level 取值（用户决定 5，2026-08-24 修订）：
 *        0 = 自动亮度（协议参数 NUL 字节 0x00 → 恢复光敏自动调光）；
 *        1~8 = 手动亮度档（协议参数 ASCII '1'~'8' = 0x31~0x38，8 最亮）。
 */
typedef struct { uint8_t level; } yn_brightness_t;

/**
 * @brief 音量设置参数（'9' 命令，'1'~'5'）。
 */
typedef struct { uint8_t level; } yn_volume_t;

/**
 * @brief 外设控制参数（'A' 命令，bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 */
typedef struct { uint8_t ctrl; } yn_peripheral_t;

/**
 * @brief 费额语音参数（'B' 命令，金额单位分）。
 */
typedef struct { uint32_t amount_fen; } yn_fee_t;

/**
 * @brief 礼貌用语播报参数（'7' 命令）。
 */
typedef struct { uint8_t idx; } yn_civil_t;

/**
 * @brief 全屏点亮参数（0x01 命令）。
 * @note  DATA0 为二进制字节，映射到 display_color_t 枚举值（恒等）：
 *        0x01 红 / 0x02 绿 / 0x03 黄（协议原文三色，保留兼容）/
 *        0x04 蓝 / 0x05 紫 / 0x06 青 / 0x07 白（用户决定 10 扩展取值，
 *        P5 户外全彩屏支持 8 色，黑色除外——全屏黑等效 '5' 全屏清除）。
 */
typedef struct { uint8_t color; } yn_fill_all_t;

/**
 * @brief 云南协议解析结果。
 */
typedef struct {
    yn_pcmd_t cmd;
    yn_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        yn_one_line_t one_line;
        yn_full_screen_t full_screen;
        yn_clear_row_t clear_row;
        yn_brightness_t brightness;
        yn_volume_t volume;
        yn_peripheral_t peripheral;
        yn_fee_t fee;
        yn_civil_t civil;
        yn_fill_all_t fill_all;
    } p;
} yn_parsed_cmd_t;

/**
 * @brief  云南协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void yn_proto_handle_task(void *argument);

/**
 * @brief  云南协议初始化入口。
 */
void yn_proto_init(void);