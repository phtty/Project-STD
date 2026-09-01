/**
 * @file    app_yn_proto_cmd.h
 * @brief   云南协议命令执行接口
 */
#pragma once

#include "app_yn_proto.h"

/**
 * @brief  执行云南协议命令。
 * @param  ch   当前通道（应答回源通道）。
 * @param  cmd  解析后的命令结构体。
 */
void yn_execute_cmd(channel_t *ch, const yn_parsed_cmd_t *cmd);