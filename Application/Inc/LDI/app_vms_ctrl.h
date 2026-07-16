#pragma once

#include "app_ldi.h"
#include "app_ldi_cmd.h"

void vms_ctrl(ldi_ctrl_vms_t *ctx, const uint16_t text_len);

/** @brief VMS 定时器轮询 — 由 ldi_timer_task 每秒调用一次，处理 keep_time 超时清屏 */
void vms_timer_poll(void);
