/**
 * @file    app_cq_proto_cmd.h
 * @brief   重庆高速二代费显协议（CQ）命令执行接口
 */

#pragma once

#include "app_dispatch.h"
#include "app_cq_proto.h"
#include "app_cq_proto_parse.h"

/**
 * @brief  执行解析后的 CQ 命令（渲染 / IO / 语音 / 网络 / 位图）。
 * @param  ch   源通道（二进制重启应答回源通道；JSON 命令无应答）。
 * @param  cmd  解析结果（sta == CQ_PARSE_OK 才执行）。
 */
void cq_execute_cmd(channel_t *ch, const cq_parsed_cmd_t *cmd);

/**
 * @brief  渲染心跳超时故障屏：
 *         「车道关闭」32pt 红（行0）+「请择道行驶!」（行2）+ 右下角红叉位图。
 * @note   布局对齐协议文档；行超出屏高自动跳过（小屏防御）。
 */
void cq_render_fault_screen(void);

/**
 * @brief  读取 screen 命令最近记录的 led 值（-1 = 未收到过）。
 * @note   screen 命令仅记录不生效（本次不含模组排布）。
 */
int cq_screen_led_get(void);