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

#define FLAG_FORCE_UPDATE      (uint32_t)(0x0000DEADU) /**< 写进 RTC 备份寄存器、指示下次启动进入升级/Recovery 的标志 */
#define FIRMWARE_MAX_FRAME_NUM (768U)                  /**< 固件最大帧数（每帧 256 字节） */
#define FLASH_SECTOR_SIZE      (0x20000U)              /**< 一个 Flash 扇区字节数（128KB） */

#define FIRMWARE_MAXLEN        (FIRMWARE_MAX_FRAME_NUM * 256U) /**< 固件最大字节数 = 帧数 × 256 */
#define BITMAP_SIZE            (FIRMWARE_MAX_FRAME_NUM / 8)    /**< 固件帧到位位图字节数（1 bit/帧） */

/** @brief IAP 返回命令码（回执帧的 cmd 字段取值） */
typedef enum {
    APP_IAP_RTN_CMD_01 = (uint32_t)0x0000b401U, /**< 报告当前 IP 配置 */
    APP_IAP_RTN_CMD_02 = (uint32_t)0x0000b402U, /**< 强制改 IP（main app 中留桩，仅回执） */
    APP_IAP_RTN_CMD_03 = (uint32_t)0x0000b403U, /**< 报告固件版本、大小、CRC32 与更新状态 */
    APP_IAP_RTN_CMD_04 = (uint32_t)0x0000b404U, /**< 固件升级准备应答 */
    APP_IAP_RTN_CMD_05 = (uint32_t)0x0000b405U, /**< 固件升级包下发应答 */
    APP_IAP_RTN_CMD_06 = (uint32_t)0x0000b406U, /**< 进入 Recovery 模式应答 */
    APP_IAP_RTN_CMD_07 = (uint32_t)0x0000b407U, /**< 软复位应答 */
} app_iap_rtn_cmd_t;

/** @brief IAP 命令处理函数指针
 *  第一参数为来源通道，第二参数为指向 IAP 帧的指针 */
typedef void (*app_iap_cmd_handler_fn_t)(app_ccb_t *, app_iap_frame_t *);

/** @brief 命令表条目数。写成显式长度而非 []：调用方要按它做范围检查，
 *  而不完整数组类型不能 sizeof。改动命令数量时这里和表定义要同步。 */
#define IAP_CMD_COUNT (8U)

extern const app_iap_cmd_handler_fn_t g_iap_cmd_table[IAP_CMD_COUNT]; /**< IAP 命令处理表，按命令码索引 */
