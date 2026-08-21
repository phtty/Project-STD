#pragma once

#include "string.h"

#include "app_rls.h"
#include "app_dispatch.h"

/* DISPLAY_SW/TMP/SAVE/PIC 四命令通用载荷：
 * TMP/SAVE 携带 bitmap[]，PIC 无 bitmap 内容（pic_num 选内置图） */
typedef struct {
    uint8_t light_level; /* 0=关屏, 1-7=开屏+固定亮度, 255=开屏+恢复自动调光 */
    uint8_t color;
    uint8_t style;
    uint8_t pic_num; /* 内置图片编号（PIC 命令） */
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
