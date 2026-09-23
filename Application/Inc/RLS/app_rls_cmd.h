#pragma once

/**
 * @file    app_rls_cmd.h
 * @brief   RLS 命令定义：显示载荷结构与命令处理表
 */

#include "string.h"

#include "app_rls.h"
#include "app_dispatch.h"

/** @brief RLS 显示载荷：亮度/颜色/风格/图片号 + 变长 1bpp 位图
 *
 *  对应显示命令（DISPLAY / DISPLAY_SAVE）DATA 域的开头；`bitmap` 用柔性数组
 *  紧跟固定头之后。 */
typedef struct {
    uint8_t light_level; /**< 亮度等级（0..7） */
    uint8_t color;       /**< 显示颜色（dev_display_color_t） */
    uint8_t style;       /**< 显示风格（协议字段） */
    uint8_t pic_num;     /**< 图片编号（协议字段） */
    uint8_t bitmap[];    /**< 1bpp 位图数据，行优先、MSB-first，长度由帧剩余量决定 */
} app_rls_display_t;

/**
 * @brief RLS 命令处理函数指针类型
 * @param ccb   通道元信息（来源通道类型、编号等）
 * @param data  指向帧 DATA 域首字节
 */
typedef void (*app_rls_cmd_handler_fn_t)(app_ccb_t *ccb, void *data);

/** RLS 命令处理函数表，按命令码索引 */
extern const app_rls_cmd_handler_fn_t g_rls_cmd_table[];
