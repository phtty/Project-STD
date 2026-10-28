#pragma once

/**
 * @file    app_lcsp_cmd.h
 * @brief   LCSP 命令定义：请求/应答码、请求上下文与命令处理表
 *
 * 命令字规则（协议 4.2）：低半字节是功能号，请求高半字节 `0x0`，应答高低半字节互换。
 * 因此**请求码与应答码必须成对登记**，不能只加请求不加应答 —— 后者会让设备收到
 * 命令后"照做了但没回"，上位机按超时重传，现场表现是命令要发三遍才生效。
 */

#include <stdint.h>

#include "app_dispatch.h"
#include "app_lcsp.h" /* app_lcsp_frame_t（请求帧里的 SEQ/CMD）*/

/* ---- 命令码（请求 → 应答）---- */

/** @brief 请求命令字（低半字节功能号） */
typedef enum {
    APP_LCSP_CMD_DEVICE_INFO = 0x01, /**< 查询设备信息 → `0x10` */
    APP_LCSP_CMD_SET_IP      = 0x02, /**< 设置 IP（只写配置，重启生效）→ `0x20` */
    APP_LCSP_CMD_QUERY_IP    = 0x03, /**< 查询 IP（应答走 UDP 广播）→ `0x30` */
    APP_LCSP_CMD_FILL_SCREEN = 0x04, /**< 全屏填充 → `0x40` */
    APP_LCSP_CMD_SHOW_IMAGE  = 0x05, /**< 显示图片（整帧）→ `0x50` */
    APP_LCSP_CMD_SHOW_SLOT   = 0x06, /**< 显示已存图片 → `0x60` */
    APP_LCSP_CMD_SET_BRIGHT  = 0x07, /**< 亮度调节 → `0x70` */
    APP_LCSP_CMD_SET_OE      = 0x08, /**< 显示开关 → `0x80` */
    APP_LCSP_CMD_REBOOT      = 0x09, /**< 重启设备 → `0x90` */
    APP_LCSP_CMD_ERROR_REPLY = 0xF0, /**< 通用错误应答（仅屏→上位机，回显原命令字）*/
    APP_LCSP_CMD_COUNT       = 0x0A, /**< 命令表长度：0x00 保留、0x0A–0x0F 未分配 */
} app_lcsp_cmd_t;

/** @brief 应答命令字（= 请求码高低半字节互换，逐条写死以便查表核对） */
typedef enum {
    APP_LCSP_REPLY_DEVICE_INFO = 0x10, /**< 查询设备信息应答 */
    APP_LCSP_REPLY_SET_IP      = 0x20, /**< 设置 IP 应答（带回读配置）*/
    APP_LCSP_REPLY_QUERY_IP    = 0x30, /**< 查询 IP 应答（广播）*/
    APP_LCSP_REPLY_FILL_SCREEN = 0x40, /**< 全屏填充应答 */
    APP_LCSP_REPLY_SHOW_IMAGE  = 0x50, /**< 显示图片应答 */
    APP_LCSP_REPLY_SHOW_SLOT   = 0x60, /**< 显示已存图片应答 */
    APP_LCSP_REPLY_SET_BRIGHT  = 0x70, /**< 亮度应答 */
    APP_LCSP_REPLY_SET_OE      = 0x80, /**< 显示开关应答 */
    APP_LCSP_REPLY_REBOOT      = 0x90, /**< 重启应答（"已受理，即将重启"）*/
} app_lcsp_reply_t;

/* ---- 状态码（协议第 9 章）---- */

/** @brief 应答状态码 */
typedef enum {
    APP_LCSP_STATUS_OK    = 0x00, /**< 成功 */
    APP_LCSP_STATUS_FAIL  = 0x01, /**< 失败，**不重传** */
    APP_LCSP_STATUS_RETRY = 0x02, /**< 失败但可重试；本模块当前不产生（本地动作没有"稍后再试"的语义）*/
} app_lcsp_status_t;

/* ---- 请求上下文 ---- */

/** @brief 一条请求的全部上下文 —— 命令处理函数的唯一入参
 *
 *  处理函数**只组应答、不发**：发与判重由任务统一做。这样"重传帧要回同样的应答"
 *  才有唯一出口（协议 4.3.4），否则每个处理函数都得自己实现一遍判重。
 */
typedef struct {
    app_ccb_t              *ccb;       /**< 请求来源通道（应答默认回这里）*/
    const app_lcsp_frame_t *frame;     /**< 完整请求帧（SEQ/CMD 从这儿取）*/
    const uint8_t          *payload;   /**< DATA 首字节（可能为 nullptr）*/
    uint16_t                payload_len; /**< DATA 字节数 */

    uint8_t        *reply_buf; /**< 应答组包区（任务提供，长度 ≤ `LCSP_REPLY_MAX_LEN`）*/
    uint16_t        reply_cap; /**< reply_buf 容量 */
    uint16_t        reply_len; /**< 组出的应答字节数；0 = 本命令不应答 */
    bool            reply_broadcast;   /**< 应答走 UDP 广播（仅 `0x03` 用）*/
    bool            reboot_after_reply; /**< 应答发出后延时复位（仅 `0x09` 用）*/
} app_lcsp_req_t;

/** @brief 命令处理函数类型
 *
 *  @param req 请求上下文；应答写进 `req->reply_buf` 并置 `req->reply_len` */
typedef void (*app_lcsp_cmd_handler_fn_t)(app_lcsp_req_t *req);

/** @brief 命令处理表，按**功能号**（命令字低半字节）索引
 *
 *  下标 0 恒为 nullptr（`0x00` 保留，互换后仍是 `0x00`，无法区分方向）。 */
extern const app_lcsp_cmd_handler_fn_t g_lcsp_cmd_table[APP_LCSP_CMD_COUNT];
