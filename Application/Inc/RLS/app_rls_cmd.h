#pragma once

/**
 * @file    app_rls_cmd.h
 * @brief   RLS 命令定义：显示载荷结构与命令处理表
 */

#include <stdbool.h>
#include "string.h"

#include "app_rls.h"
#include "app_dispatch.h"

/** @brief RLS 显示载荷：亮度/颜色/风格/图片号 + 变长 1bpp 位图
 *
 *  对应显示命令（DISPLAY / DISPLAY_SAVE）DATA 域的开头；`bitmap` 用柔性数组
 *  紧跟固定头之后。 */
typedef struct {
    uint8_t light_level; /**< 亮度，**线上口径** 0~31（0 最暗、31 最亮）/ 255 自动；
                              handler 压到面板 1~7 档或恢复光感跟随 */
    uint8_t color;       /**< 显示颜色，**线上口径**，与 `dev_display_color_t` 同序
                              （0 黑 / 1 红 / 2 绿 / 3 黄…7 白，旧实现实测口径）；
                              > 7 整帧拒显，见 app_rls_cmd.c */
    uint8_t style;       /**< 显示风格（协议字段） */
    uint8_t pic_num;     /**< 图片编号（协议字段） */
    uint8_t bitmap[];    /**< 1bpp 位图数据，行优先、MSB-first，长度由帧剩余量决定 */
} app_rls_display_t;

/** @brief 修改 IP 载荷：IP[4] | 子网掩码[4] | 网关[4]
 *
 *  协议帧里**没有端口字段**（文档那张表还留了一行没偏移的"别名"，21 字节装不下，
 *  按笔误处理）—— 端口沿用设备当前配置，见 app_rls_cmd.c 的 `_rls_cmd_set_ip`。 */
typedef struct {
    uint8_t ip[4];   /**< 设备 IP */
    uint8_t mask[4]; /**< 子网掩码 */
    uint8_t gw[4];   /**< 默认网关 */
} app_rls_net_cfg_t;

/** @brief 搜索应答（REPORT_IP）定长：帧头 6 + 地址 12 + BCC 1 + 帧尾 2 */
#define RLS_REPORT_IP_LEN (21U)

/**
 * @brief RLS 命令处理函数指针类型
 * @param ccb      通道元信息（来源通道类型、编号等）
 * @param data     指向帧 DATA 域首字节
 * @param data_len **整帧**字节数（`app_dispatch_msg_t.data_len`，即帧内长度域）。
 *                 这是**内部签名**、不是线上格式；处理器用它换算帧内还能带多少载荷
 *                 （如显示帧的位图可用字节），以便在越界读之前拒绝。
 */
typedef void (*app_rls_cmd_handler_fn_t)(app_ccb_t *ccb, void *data, uint16_t data_len);

/** RLS 命令处理函数表，按命令码索引 */
extern const app_rls_cmd_handler_fn_t g_rls_cmd_table[];

/** @brief 该命令是否需要**回显原帧**（文档 §5"收到什么指令反馈什么指令"）
 *
 *  例外是：
 *   · **发图并显示**的两条（BM / SV）：上位机只看屏，回一帧 525 字节上去纯属给
 *     总线/网络加负担，且这两条本身没有任何应答语义（现场约定）；
 *   · **查询类**（搜索 ST/SC，以及将来实现 CK）：只发它自己的应答帧，不再回显 ——
 *     否则一次查询回两帧。
 *  未知命令码也不回显 —— 那不是"收到什么回什么"，而是把一个不认识的帧原样播回去。
 *
 *  @param cmd 线上命令码（小端拼成的 uint16，取值见 app_rls_cmd_type_t）
 *  @return true = 处理完把收到的原帧原样回给来源 */
bool app_rls_cmd_should_echo(app_rls_cmd_type_t cmd);
