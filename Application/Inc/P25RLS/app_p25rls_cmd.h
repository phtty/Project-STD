#pragma once

/**
 * @file    app_p25rls_cmd.h
 * @brief   P25RLS 命令定义：各命令的 DATA 结构、命令表与处理函数签名
 *
 * 命令表**按命令字查表**（不按位置）：新增命令只加一行 + 把 `P25RLS_CMD_CNT` 加一，
 * 没有"两张按位置对应的表"那种错一位就整族错派的风险。
 */

#include <stdint.h>

#include "app_dispatch.h"
#include "app_p25rls.h"

/** @brief 位图宽度（点）：文档 96×24 的屏 */
#define P25RLS_BITMAP_W (96U)

/** @brief 位图高度（行） */
#define P25RLS_BITMAP_H (24U)

/** @brief 单个颜色平面的字节数：ceil(96/8) × 24 = 12 × 24 = 288
 *
 *  文档写"1 个点 1 byte"，但同一张表又给 `24*96/8*2 = 576` —— 按后者：
 *  **每像素 1 bit**，先红平面、后绿平面，各 288B。 */
#define P25RLS_PLANE_BYTES (12U * P25RLS_BITMAP_H)

/** @brief `SV` 位图总字节数 = 红平面 + 绿平面 */
#define P25RLS_BITMAP_BYTES (2U * P25RLS_PLANE_BYTES)

/** @brief `SV` 载荷字节数：亮度(1) + 图片编号(1) + 位图(576) */
#define P25RLS_DISPLAY_PAYLOAD_BYTES (2U + P25RLS_BITMAP_BYTES)

/** @brief `IP` / `SC` / `RB` 的 DATA 字节数：IP(4) + 掩码(4) + 网关(4)，**没有端口字段** */
#define P25RLS_NET_CFG_BYTES (12U)

/** @brief `RB` 应答整帧字节数：帧头 6 + 地址 12 + BCC 1 + 帧尾 2 */
#define P25RLS_REPORT_IP_BYTES (21U)

/** @brief `SV` 载荷：亮度 + 图片编号 + 双平面 1bpp 位图 */
typedef struct [[gnu::packed]] {
    uint8_t brightness; /**< 亮度，**线上口径** 0~31（0 最暗、31 最亮）/ 255 自动 */
    uint8_t pic_num;    /**< 图片编号 0/1/2（= 图片槽号） */
    uint8_t bitmap[];   /**< 先红平面 288B、后绿平面 288B；每像素 1bit、行优先、MSB 为最左 */
} app_p25rls_display_t;

/** @brief `IP` / `SC` / `RB` 的网络配置：IP + 掩码 + 网关（协议帧里没有端口字段） */
typedef struct [[gnu::packed]] {
    uint8_t ip[4];   /**< 设备 IP */
    uint8_t mask[4]; /**< 子网掩码 */
    uint8_t gw[4];   /**< 默认网关 */
} app_p25rls_net_cfg_t;

/** @brief `HZ` 载荷：颜色 + 点阵大小 + GBK 文本 */
typedef struct [[gnu::packed]] {
    uint8_t color;     /**< 颜色：1 红 / 2 绿 / 3 黄 */
    uint8_t dot_size;  /**< 点阵大小：0 = 16 / 1 = 24 / 2 = 32（协议没有字型字段） */
    uint8_t text[];    /**< GBK 文本，可含 '\n' */
} app_p25rls_text_t;

/** @brief 命令处理函数指针
 *  @param ccb         本帧来源通道（应答发往它）
 *  @param payload     DATA 域首字节
 *  @param payload_len DATA 域字节数（不含 BCC 与帧尾） */
typedef void (*app_p25rls_cmd_handler_fn_t)(app_ccb_t *ccb, const void *payload,
                                            uint16_t payload_len);

/** @brief 命令表项：命令字与其处理函数 */
typedef struct {
    app_p25rls_cmd_type_t cmd;              /**< 命令字 */
    app_p25rls_cmd_handler_fn_t handler_fn; /**< 处理函数 */
} app_p25rls_cmd_entry_t;

/** @brief 命令表项数（与 `app_p25rls.c` 里的 static_assert 一起钉住表长） */
#define P25RLS_CMD_CNT (7U)

/** @brief 命令表；只含**主机→设备**方向（`RB` 是应答，不在表里） */
extern const app_p25rls_cmd_entry_t g_p25rls_cmd_table[P25RLS_CMD_CNT];
