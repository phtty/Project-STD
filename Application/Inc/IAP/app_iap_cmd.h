#pragma once

/**
 * @file    app_iap_cmd.h
 * @brief   IAP 命令定义：返回命令码与命令处理表
 */

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
    APP_IAP_RTN_CMD_01 = (uint32_t)0x0000b401U,
    APP_IAP_RTN_CMD_02 = (uint32_t)0x0000b402U,
    APP_IAP_RTN_CMD_03 = (uint32_t)0x0000b403U,
    APP_IAP_RTN_CMD_04 = (uint32_t)0x0000b404U,
    APP_IAP_RTN_CMD_05 = (uint32_t)0x0000b405U,
    APP_IAP_RTN_CMD_06 = (uint32_t)0x0000b406U,
    APP_IAP_RTN_CMD_07 = (uint32_t)0x0000b407U,
} app_iap_rtn_cmd_t;

typedef void (*app_iap_cmd_handler_fn_t)(app_ccb_t *, app_iap_frame_t *);

/** 命令表条目数。写成显式长度而非 []：调用方要按它做范围检查，
 *  而不完整数组类型不能 sizeof。改动命令数量时这里和表定义要同步。 */
#define IAP_CMD_COUNT (8U)

extern const app_iap_cmd_handler_fn_t g_iap_cmd_table[IAP_CMD_COUNT];
