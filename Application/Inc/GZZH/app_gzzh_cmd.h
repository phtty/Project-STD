#pragma once

/**
 * @file    app_gzzh_cmd.h
 * @brief   GZZH 命令定义：基本数据包结构、命令表与处理函数签名
 *
 * 一张命令表（命令字 → 处理函数）**按命令字查找**，不按位置——RLS 那种
 * "两张按位置对应的表"错一位就错派一整个命令，这里从结构上避免。
 */

#include <stdint.h>

#include "app_dispatch.h"
#include "app_gzzh.h"

/** @brief 0x10 应答字节数：**裸 2 字节**状态（无帧头帧尾，参考实现口径） */
#define GZZH_STATUS_BYTES (2U)

/** @brief 0x40 应答（0x50）整帧字节数：引导段 16 + 载荷 14 + 结束符 1 */
#define GZZH_REPORT_IP_BYTES (31U)

/** @brief 0x80 全屏纯色载荷字节数：R / G / B 各 1 字节 */
#define GZZH_FILL_BYTES (3U)

/** @brief 显示文字载荷（0x20）—— 17 字节固定头 + GBK 文本
 *
 *  字段顺序按**文档**（屏宽在屏高之前）。参考实现把这两个 2 字节字段命名为
 *  `high`/`width` 且顺序相反，但它两个都没用 —— 本协议解析器同样不用（几何一律
 *  取本机逻辑屏），所以两种读法不影响任何行为。
 *
 *  文本是 GBK（与字库同编码），可含 `\n`；不含结尾 NUL，长度 = 帧长度 − 34。 */
typedef struct [[gnu::packed]] {
    uint8_t x[2];         /**< 文字左上角 X，小端（像素） */
    uint8_t y[2];         /**< 文字左上角 Y，小端（像素；参考实现只取低字节） */
    uint8_t screen_w[2];  /**< 屏宽（主机视角，仅参考，本机不使用） */
    uint8_t screen_h[2];  /**< 屏高（主机视角，仅参考，本机不使用） */
    uint8_t font_name[4]; /**< 字体名称，GBK 两字：宋体 / 黑体 / 楷体 / 仿宋 */
    uint8_t font_size[2]; /**< 字体大小：首字节为字号（像素高，或 0~3 序号） */
    uint8_t color[3];     /**< 字体颜色 R / G / B（非 0 即该通道亮） */
    uint8_t text[];       /**< GBK 文本，可含 '\n' */
} app_gzzh_text_t;

/** @brief 设置 IP 载荷（0x40）：地址 + 掩码 + 网关 + 端口 */
typedef struct [[gnu::packed]] {
    uint8_t ip[4];   /**< 设备 IP */
    uint8_t mask[4]; /**< 子网掩码 */
    uint8_t gw[4];   /**< 默认网关 */
    uint8_t port[2]; /**< 端口，**大端**（参考实现口径；本协议其余字段都是小端） */
} app_gzzh_net_cfg_t;

/** @brief 命令处理函数指针
 *  @param ccb         本帧来源通道（应答发往它）
 *  @param payload     基本数据包首字节
 *  @param payload_len 基本数据包字节数（**不含**结束符） */
typedef void (*app_gzzh_cmd_handler_fn_t)(app_ccb_t *ccb, const uint8_t *payload,
                                          uint16_t payload_len);

/** @brief 命令表项：命令字与其处理函数 */
typedef struct {
    app_gzzh_cmd_type_t cmd;              /**< 命令字（低 8 位有效） */
    app_gzzh_cmd_handler_fn_t handler_fn; /**< 处理函数 */
} app_gzzh_cmd_entry_t;

/** @brief 命令表项数（与 `app_gzzh.c` 里的 static_assert 一起钉住表长） */
#define GZZH_CMD_CNT (5U)

/** @brief 命令表；只含**主机→设备**方向，0x50（应答）不在其中 */
extern const app_gzzh_cmd_entry_t g_gzzh_cmd_table[GZZH_CMD_CNT];
