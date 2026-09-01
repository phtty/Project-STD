/**
 * @file    app_sc_etc_proto.h
 * @brief   四川 ETC 费显协议（1D）公共定义
 *
 * 帧格式：`0x0A + 显示方式(00静态/01滚屏) + 行号(00全屏/01~06) + 数据 + 0x0D`
 * 灯控：`0A 36/37/38/39 0D`（红/绿/黄闪开/黄闪关；黄闪开 10 秒自动关闭）
 * 亮度：`0A 40 XX YY 0D`（XX 00~07 亮度，00 最暗、07 最亮，YY 保留）；心跳：`0A 50 0D`
 * 上行应答：`0A 00 0D` 正常 / `0A 01 0D` 数据超长 / `0A 02 0D` 帧错误。
 * 数据变长 0x0D 定界（无固定 56B 全屏），单行 ≤24B、全屏 ≤145B；
 * 0x20 清屏（行号 0 清全屏）；0x30 初始化 → 软件复位；显示颜色跟随通行灯状态。
 */
#pragma once

#include <stdint.h>
#include "app_dispatch.h"

/**
 * @brief ETC 协议命令类型。
 */
typedef enum {
    SC_ETC_PCMD_DISPLAY = 0,     /**< 显示命令（静态/滚屏，行号细分） */
    SC_ETC_PCMD_LIGHT_RED,       /**< 交通灯红 0A 36 0D */
    SC_ETC_PCMD_LIGHT_GREEN,     /**< 交通灯绿 0A 37 0D */
    SC_ETC_PCMD_LIGHT_YELLOW_ON, /**< 黄闪报警开 0A 38 0D */
    SC_ETC_PCMD_LIGHT_YELLOW_OFF,/**< 黄闪报警关 0A 39 0D */
    SC_ETC_PCMD_BRIGHTNESS,      /**< 亮度设置 0A 40 XX YY 0D */
    SC_ETC_PCMD_HEARTBEAT,       /**< 心跳 0A 50 0D */
    SC_ETC_PCMD_INVALID,
} sc_etc_pcmd_t;

/**
 * @brief ETC 协议帧解析状态。
 */
typedef enum {
    SC_ETC_PARSE_OK = 0,
    SC_ETC_PARSE_ERR_FRAME,   /**< 帧头/帧尾错误 */
    SC_ETC_PARSE_ERR_CMD,     /**< 命令编号不正确 */
    SC_ETC_PARSE_ERR_PARAM,   /**< 参数非法 */
    SC_ETC_PARSE_ERR_TOOLONG, /**< 数据超长（单行 >24 / 全屏 >145 字节） */
} sc_etc_parse_sta_t;

/**
 * @brief 显示命令参数。
 */
typedef struct {
    uint8_t mode;          /**< 0=静态显示，1=滚屏显示 */
    uint8_t row;           /**< 0=全屏，1~6=第 1~6 行 */
    const uint8_t *text;   /**< 显示数据（GBK） */
    uint16_t text_len;     /**< 数据字节数 */
} sc_etc_display_t;

/**
 * @brief 亮度设置参数（0~7，0 最暗 7 最亮）。
 */
typedef struct {
    uint8_t level;
} sc_etc_brightness_t;

/**
 * @brief ETC 协议解析结果。
 */
typedef struct {
    sc_etc_pcmd_t cmd;
    sc_etc_parse_sta_t sta;
    uint16_t data_len;
    const uint8_t *data;
    union {
        sc_etc_display_t display;
        sc_etc_brightness_t brightness;
    } p;
} sc_etc_parsed_cmd_t;

/**
 * @brief  解析 ETC 协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
sc_etc_parsed_cmd_t sc_etc_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  执行 ETC 协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void sc_etc_execute_cmd(channel_t *ch, const sc_etc_parsed_cmd_t *cmd);

/**
 * @brief  心跳超时渲染「ETC车道关闭，请择道行驶」（由计时任务调用）。
 * @note   2026-08-17：心跳计时任务已停用（#if 0），本函数暂无调用方，保留待恢复。
 */
void sc_etc_show_lane_closed(void);

/**
 * @brief  黄闪 10 秒自动关闭计时启动（0x38 收到时调用）。
 * @note   2026-08-17：心跳计时任务已停用，计时不再被消费——黄闪开启后不再
 *         10 秒自动关闭，须由 0A 39 显式关闭；本函数仍被调用仅记录状态。
 */
void sc_etc_hs_timer_arm(void);

/**
 * @brief  黄闪 10 秒自动关闭计时取消（0x39 收到时调用）。
 */
void sc_etc_hs_timer_cancel(void);

/**
 * @brief  黄闪 10 秒自动关闭到期回调（由心跳计时任务调用）。
 * @note   2026-08-17：心跳计时任务已停用，本函数暂无调用方，保留待恢复。
 */
void sc_etc_hs_timeout(void);

/**
 * @brief  ETC 协议处理任务入口。
 * @param  argument  任务参数，当前未使用。
 */
void sc_etc_proto_handle_task(void *argument);

/**
 * @brief  ETC 协议初始化入口。
 */
void sc_etc_proto_init(void);