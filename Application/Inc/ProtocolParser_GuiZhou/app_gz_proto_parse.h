/**
 * @file    app_gz_proto_parse.h
 * @brief   贵州协议帧解析接口
 */
#pragma once

#include "app_gz_proto.h"

/**
 * @brief  解析贵州协议原始帧。
 * @param  raw      原始帧数据。
 * @param  raw_len  原始帧长度。
 * @return 解析后的命令结构体；状态字段 sta 标识解析结果。
 */
gz_parsed_cmd_t gz_parse_frame(const uint8_t *raw, uint16_t raw_len);

/**
 * @brief  金额 ASCII 数字串转分（整数运算，无浮点）。
 * @param  str  金额数字串（整数或带小数，如 "1.20"）。
 * @param  len  数字串字节数。
 * @return 金额（分）= 整数部分×100 + 小数前两位（%.2f 语义，第三位起截断）。
 * @note   '6' 固定格式的金额/余额/总重/超重字段复用本函数转分/吨分，
 *         车型字段取返回值 ÷100 得整数车型号。
 */
uint32_t gz_amount_to_fen(const uint8_t *str, uint16_t len);