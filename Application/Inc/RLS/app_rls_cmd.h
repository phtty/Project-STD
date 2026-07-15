#pragma once

#include "string.h"

#include "app_rls.h"
#include "app_dispatch.h"

typedef struct {
    uint8_t light_level;
    uint8_t color;
    uint8_t style;
    uint8_t pic_num;
    uint8_t bitmap[];
} rls_dispaly_t;

/**
 * RLS 命令处理函数指针类型
 * @param meta  通道元信息（来源通道类型、编号等）
 * @param data  指向帧 DATA 域首字节
 */
typedef void (*rls_cmd_handler_fn_t)(channel_t *, void *);

/** RLS 命令处理函数表，按命令码索引 */
extern const rls_cmd_handler_fn_t g_rls_cmd_table[];
