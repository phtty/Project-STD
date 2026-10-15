#pragma once

/**
 * @file    app_cdstd_cmd.h
 * @brief   CDSTD 命令定义：复合指令载荷结构与命令处理表
 */

#include <stdio.h>
#include <stdint.h>

#include "app_cdstd.h"

#include "app_dispatch.h"

/**
 * @brief 命令处理函数指针类型
 *
 * @param ccb       通道控制块
 * @param data      帧data域首字节
 * @param data_len  data域长度
 */
typedef void (*app_cdstd_cmd_handler_fn_t)(app_ccb_t *ccb, void *data, uint16_t data_len);

extern const app_cdstd_cmd_handler_fn_t g_cdstd_cmd_table[];

typedef enum {
    CDSTD_CMD_SCREEN_SWITCH,      // 屏开关
    CDSTD_CMD_WORD_DISPLAY,       // 文字显示
    CDSTD_CMD_LIGHT_ADJUST,       // 亮度调节
    CDSTD_CMD_FILL_SCREEN,        // 全屏填充
    CDSTD_CMD_PERSISTENT_DISPLAY, // 半屏持久化
    CDSTD_CMD_SET_IP,             // 设置ip地址
    CDSTD_CMD_REPORT_IP,          // 上报ip地址
    CDSTD_CMD_PICTRUE_DISPLAY,    // 图片
    CDSTD_CMD_RELEASE_DISPLAY,    // 发布盒显示
    CDSTD_CMD_COUNT,              // 命令总数
} app_cdstd_cmd_t;

typedef struct [[gnu::packed]] {
    uint16_t head;
    uint8_t ver;
    uint8_t seq;
    uint8_t reserved[2];
    uint8_t len[2];
    uint8_t cmd;
    uint8_t status;
    uint8_t crc[2];
} app_cdstd_reply_status_t;

typedef struct [[gnu::packed]] {
    uint16_t head;
    uint8_t ver;
    uint8_t seq;
    uint8_t reserved[2];
    uint8_t len[2];
    uint8_t cmd;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint8_t port[2];
    uint8_t crc[2];
} app_cdstd_reply_ip_t;
