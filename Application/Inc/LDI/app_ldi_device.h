/**
 * @file    app_ldi_device.h
 * @brief   LDI 设备能力层 — 协议解析与硬件执行解耦
 *
 * 协议层 (app_ldi_cmd) 只负责校验/分发/回包；
 * E6/E7/E8 的实际执行与状态缓存统一由此层提供。
 *
 * 硬件/协议兼容约定：
 *   E7 控制 Color: 01H绿 / 02H红 / 03H黄
 *   E7 0CH 上报(附录 A.1): 00H红灯 / 01H绿灯（无黄）
 *       黄灯硬件降级关灯，上报按红灯 00H
 *   E8 报警器：暂由 Flash_Light 代实现；WorkMode 频率用软闪烁
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "app_ldi_cmd.h"

/** 控制执行结果：00H=成功, 01H=失败（与 B1H status 一致） */
typedef enum {
    LDI_DEV_OK   = 0x00,
    LDI_DEV_FAIL = 0x01,
} ldi_dev_result_t;

/** E6 显示运行状态缓存（附录 A.1: 00H关闭 / 01H开启） */
typedef struct {
    uint8_t running_status;
    uint8_t font_color;
    uint8_t keep_time;
} ldi_dev_display_sta_t;

/** E7 信号灯状态
 *  color: 最近一次控制色码 01绿/02红/03黄
 *  running_status: 附录 A.1 上报码 00红 / 01绿
 */
typedef struct {
    uint8_t running_status;
    uint8_t color;
} ldi_dev_signal_sta_t;

/** E8 报警器状态（附录 A.1: 00H停止 / 01H报警中） */
typedef struct {
    uint8_t running_status;
    uint8_t work_mode;
    uint8_t keep_time;
} ldi_dev_alarm_sta_t;

void ldi_device_init(void);

/** E6 显示控制/清屏；text_len 为 text[] 有效字节数（不含定长头） */
ldi_dev_result_t ldi_device_display_ctrl(const ldi_ctrl_display_t *ctrl, uint16_t text_len);

/** E7 通行信号灯颜色控制 */
ldi_dev_result_t ldi_device_lane_signal_ctrl(const ldi_ctrl_signal_t *ctrl);

/** E8 报警器开关控制（含 work_mode 频率闪烁 / keep_time 定时关闭） */
ldi_dev_result_t ldi_device_alarm_ctrl(const ldi_ctrl_alarm_t *ctrl);

/** 由 ldi_timer_task 每秒调用：E6 KeepTime / E8 KeepTime+频率闪烁 */
void ldi_device_timer_poll(void);

/** 填充 0CH 上报用的单模块状态（仅写 running_status；附录 A.1 编码） */
bool ldi_device_fill_sta(uint8_t device_type, ldi_device_info_t *info);

const ldi_dev_display_sta_t *ldi_device_display_sta(void);
const ldi_dev_signal_sta_t *ldi_device_lane_signal_sta(void);
const ldi_dev_alarm_sta_t *ldi_device_alarm_sta(void);
