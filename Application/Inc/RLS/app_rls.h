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
typedef enum {
    APP_RLS_CMD_TYPE_TEST         = 0x0000, /**< 测试命令 */
    APP_RLS_CMD_TYPE_DISPLAY      = 0x4d42, /**< 显示命令 */
    APP_RLS_CMD_TYPE_DISPLAY_SAVE = 0x5653, /**< 显示并保存命令 */
} app_rls_cmd_type_t;

extern osMessageQueueId_t g_rls_msg_queue;   /**< RLS 帧队列 */
extern osThreadId_t g_rls_task_handle;       /**< RLS 协议任务句柄 */
extern const osThreadAttr_t g_rls_task_attr; /**< RLS 协议任务属性 */

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
