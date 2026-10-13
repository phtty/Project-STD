#pragma once

/**
 * @file    app_ahmq.h
 * @brief   AHMQ 应用层协议：MQTT 帧、主题/通知结构与协议控制块
 */

#include <stdint.h>

#include "cmsis_os2.h"
#include "ring_buffer.h"

#include "app_dispatch.h"

#define MQTT_FRAME_MIN_LEN (21U)        /**< 最短 MQTT 帧字节数 */
#define MQTT_FRAME_MAX_LEN (21U + 512U) /**< 最长 MQTT 帧字节数 */

/** @brief MQTT 主题四段标识（站点/车道/设备类型/设备号） */
typedef struct [[gnu::packed]] app_ahmq_topic_info {
    char station_hex[8]; /**< 站点编号（HEX 字符串） */
    char lane_hex[2];    /**< 车道编号 */
    char device_type[2]; /**< 设备类型 */
    char device_id[2];   /**< 设备编号 */
} app_ahmq_topic_info_t;

/** @brief 通知号中的日期时间（年/月/日/时） */
typedef struct [[gnu::packed]] app_ahmq_notify_date {
    char year[4];  /**< 年 */
    char month[2]; /**< 月 */
    char day[2];   /**< 日 */
    char hour[2];  /**< 时 */
} app_ahmq_notify_date_t;

/** @brief 通知号（日期时间 + 设备标识 + 发送计数） */
typedef struct [[gnu::packed]] app_ahmq_notify_id {
    app_ahmq_notify_date_t date_time; /**< 日期时间 */
    char device_type[2];              /**< 设备类型 */
    char device_id[2];                /**< 设备编号 */
    char send_count[6];               /**< 发送计数 */
} app_ahmq_notify_id_t;

// 设备状态
/** @brief 设备状态上报载荷 */
typedef struct [[gnu::packed]] app_ahmq_state_report {
    app_ahmq_notify_id_t notify; /**< 通知号 */
    char work_status;            /**< 工作状态 */
    char run_status;             /**< 运行状态 */
    char reserved[9];            /**< 保留 */
} app_ahmq_state_report_t;

// 设备签到
/** @brief 设备签到载荷 */
typedef struct [[gnu::packed]] app_ahmq_sign_up {
    app_ahmq_notify_id_t notify; /**< 通知号 */
    char type;                   /**< 签到类型 */
    char work_status;            /**< 工作状态 */
    char soft_ver[10];           /**< 软件版本 */
    char hard_ver[10];           /**< 硬件版本 */
    char protocol_ver[10];       /**< 协议版本 */
    char company[10];            /**< 厂商 */
    char device[10];             /**< 设备型号 */
    char reserved[21];           /**< 保留 */
} app_ahmq_sign_up_t;

/**
 * @brief AHMQ 协议子类
 *
 * base 必须是第一个成员（container_of 偏移 0）。协议自有状态收在这里，不再散落成
 * 文件级全局 —— 探针、任务与命令处理都经 base 取回，多实例也因此成为可能。
 */
typedef struct {
    app_pcb_t        base;                 /**< 协议基类，必须为首成员（container_of 前提） */
    app_ahmq_topic_info_t topic_info;      /**< 设备标识，用于拼接主题 */
    app_ahmq_notify_id_t  notify_id;       /**< 上报帧携带的通知号（对时命令会更新） */
    char         reply_topic[64]; /**< 本次回复的目的主题，随消息交给通道 */
} app_ahmq_proto_t;

extern osMessageQueueId_t g_ahmq_msg_queue;   /**< AHMQ 帧队列 */
extern osThreadId_t g_ahmq_task_handle;       /**< AHMQ 协议任务句柄 */
extern const osThreadAttr_t g_ahmq_task_attr; /**< AHMQ 协议任务属性 */

/** @brief AHMQ 协议任务入口 */
void app_ahmq_task(void *argument);

/** @brief AHMQ 帧探测（pcb_ops.probe）
 * 契约见 app_dispatch.h 的 app_pcb_probe_fn_t
 *
 * 一条 MQTT 消息即一帧，以结尾 NUL 定界；按来源主题映射到本协议命令号。
 *
 * @param self           协议控制块
 * @param ccb            数据来源通道（本协议不使用）
 * @param src            本帧来源描述，用于判定来源主题
 * @param[out] scratch   框架暂存区；本函数写入窥视到的帧字节
 * @param scratch_size   暂存区容量；帧长超过它时返回 SKIP，不得越界写
 * @param[out] total_len 输出：完整帧长度（含结尾 NUL）
 * @param[out] aux       输出：协议层命令号（READY 时有效）
 * @return 探测状态，取值见 app_pcb_probe_state_t
 */
app_pcb_probe_state_t app_ahmq_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                    uint8_t *scratch, uint16_t scratch_size,
                                    uint32_t *total_len, uint8_t *aux);
