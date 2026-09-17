#pragma once

#include <stdint.h>

#include "app_iap.h"
#include "app_dispatch.h"
#include "app_udp.h"
#include "app_iap_cfg.h"

#define FLAG_FORCE_UPDATE      (uint32_t)(0x0000DEADU)
#define FIRMWARE_MAX_FRAME_NUM (768U)
#define FLASH_SECTOR_SIZE      (0x20000U)

#define FIRMWARE_MAXLEN        (FIRMWARE_MAX_FRAME_NUM * 256U)
#define BITMAP_SIZE            (FIRMWARE_MAX_FRAME_NUM / 8)

typedef enum {
    rtn_cmd01 = (uint32_t)0x0000b401U,
    rtn_cmd02 = (uint32_t)0x0000b402U,
    rtn_cmd03 = (uint32_t)0x0000b403U,
    rtn_cmd04 = (uint32_t)0x0000b404U,
    rtn_cmd05 = (uint32_t)0x0000b405U,
    rtn_cmd06 = (uint32_t)0x0000b406U,
    rtn_cmd07 = (uint32_t)0x0000b407U,
} rtn_cmd_t;

typedef void (*iap_cmd_handler_fn_t)(ccb_t *, iap_frame_t *);

/** 命令表条目数。写成显式长度而非 []：调用方要按它做范围检查，
 *  而不完整数组类型不能 sizeof。改动命令数量时这里和表定义要同步。 */
#define IAP_CMD_COUNT (8U)

extern const iap_cmd_handler_fn_t g_iap_cmd_table[IAP_CMD_COUNT];
