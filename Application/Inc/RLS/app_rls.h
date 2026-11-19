#pragma once

/**
 * @file    app_rls.h
 * @brief   RLS 协议：帧格式、命令类型与任务接口
 */

#include "string.h"

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief RLS 帧 — 帧头 6 字节 + DATA/BCC + 帧尾 */
typedef struct {
    uint8_t head[2];         /**< 帧头，固定 0xFF 0xFE */
    uint8_t length[2];       /**< 整帧字节数（含头含尾），大端 */
    uint8_t cmd[2];          /**< 命令类型，大端 */
    uint8_t data_bcc_tail[]; /**< DATA + BCC 校验 + 帧尾 0x0D 0x0C */
} app_rls_frame_t;

/** @brief RLS 命令类型 */
/** @brief RLS 命令类型
 *
 *  **命令码按"线上两个字节按小端拼成 uint16"取值**：线上 `0x49 0x50`（ASCII "IP"）
 *  在本枚举里是 `0x5049`。探测端用 `*(app_rls_cmd_type_t *)frame->cmd` 读，
 *  ARM 小端 + `-fshort-enums`（16 位）下正好等价于这两个字节。
 *
 *  文档里有两处命令码自相矛盾，取舍见各项注释。 */
typedef enum {
    APP_RLS_CMD_TYPE_TEST          = 0x0000, /**< 测试命令（文档未列，沿用既有实现） */
    APP_RLS_CMD_TYPE_DISPLAY       = 0x4d42, /**< 全屏图片显示（线上 "BM"） */
    APP_RLS_CMD_TYPE_DISPLAY_SAVE  = 0x5653, /**< 显示并保存（线上 "SV"） */
    APP_RLS_CMD_TYPE_SET_IP        = 0x5049, /**< 修改 IP（线上 "IP" = 0x49 0x50） */
    APP_RLS_CMD_TYPE_AQUIRY_IP     = 0x5453, /**< 搜索设备（线上 "ST" = 0x53 0x54，
                                              *   取文档"数值"列；"描述"列写作 SC） */
    APP_RLS_CMD_TYPE_AQUIRY_IP_SC  = 0x4353, /**< 搜索设备的另一种读法（线上 "SC" =
                                              *   0x53 0x43）。两种都收，应答一致 ——
                                              *   文档这里描述与数值各写一套，见 app_rls.c */
    APP_RLS_CMD_TYPE_REPORT_IP     = 0x4252, /**< 搜索应答（线上 "RB" = 0x52 0x42）——
                                              *   **仅设备→主机方向**，不进接收分派表 */
} app_rls_cmd_type_t;

/** @brief RLS 协议任务入口 */
void app_rls_task(void *argument);

/** @brief RLS 帧探测（pcb_ops.probe）
 * 契约见 app_dispatch.h 的 app_pcb_probe_fn_t
 *
 * @param self           协议控制块
 * @param ccb            数据来源通道（本协议不使用）
 * @param src            本帧来源描述（本协议不使用）
 * @param[out] scratch   框架暂存区；本函数写入窥视到的帧字节
 * @param scratch_size   暂存区容量；帧长超过它时返回 SKIP，不得越界写
 * @param[out] total_len 输出：完整帧长度（READY / SKIP 时有效）
 * @param aux            协议层分类输出；本实现不向协议任务传分类，不写此参数
 * @return 探测状态，取值见 app_pcb_probe_state_t
 */
app_pcb_probe_state_t app_rls_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                uint8_t *aux);
