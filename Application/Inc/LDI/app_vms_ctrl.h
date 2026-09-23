#pragma once

/**
 * @file    app_vms_ctrl.h
 * @brief   VMS 情报板控制：文本显示与定时清屏接口
 */

#include "app_ldi.h"
#include "app_ldi_cmd.h"

void app_vms_ctrl(app_ldi_ctrl_vms_t *ctx, const uint16_t text_len);

/** @brief VMS 定时器轮询 — 由 app_ldi_timer_task 每秒调用一次，处理 keep_time 超时清屏 */
void app_vms_timer_poll(void);
