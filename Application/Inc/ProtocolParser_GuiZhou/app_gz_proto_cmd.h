/**
 * @file    app_gz_proto_cmd.h
 * @brief   贵州协议命令执行接口
 */
#pragma once

#include "app_gz_proto.h"

/**
 * @brief  执行贵州协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void gz_execute_cmd(channel_t *ch, const gz_parsed_cmd_t *cmd);