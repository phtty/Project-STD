/**
 * @file    dev_mqtt.h
 * @brief   MQTT 客户端通道（Device 层）
 *
 * 连接 MQTT Broker，订阅配置主题，接收数据通过 app_channel_dispatch 写入调度框架。
 * 默认 Broker: 120.46.136.199:6000, Client ID: "CD_ZTP"
 * 当前状态：任务未创建（InitTask 中注释）
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief MQTT 连接状态机 */
typedef enum {
    MQTT_ST_DISCONNECTED, /**< 未连接 */
    MQTT_ST_CONNECTING,   /**< TCP + MQTT CONNECT 进行中 */
    MQTT_ST_CONNECTED,    /**< MQTT CONNACK 已接受，等待 subscribe */
    MQTT_ST_READY,        /**< subscribe 完成，可正常收发 */
} mqtt_state_t;

/** @brief MQTT 运行上下文 — 散落全局资源的聚合 */
typedef struct {
    void *client;                /**< 不透明句柄（中间件 mqtt_client），在 .c 中 cast 回具体类型 */
    uint8_t broker_ip[4];        /**< Broker IP 地址 */
    uint16_t broker_port;        /**< Broker 端口 */
    char client_id[32];          /**< MQTT Client ID */
    char client_user[32];        /**< MQTT 用户名 */
    char client_pass[32];        /**< MQTT 密码 */
    osSemaphoreId_t connect_sem; /**< CONNACK 同步信号量 */
    uint8_t rcv_buf[1044];       /**< 接收缓冲区 */
    uint32_t payload_offset;     /**< rcv_buf 写入偏移 */
} mqtt_ctx_t;

/** @brief MQTT 通道子类（单例，channel_t 为第一个成员） */
typedef struct {
    channel_t me;
    mqtt_state_t state; /**< MQTT 连接状态机 */
    mqtt_ctx_t ctx;     /**< 运行时上下文 */
    char topic[64];
    uint16_t payload_len; /**< 当前帧 payload 长度（probe 函数使用） */
} mqtt_channel_t;

extern const ch_ops_t mqtt_ch_ops;
extern channel_t g_mqtt_channel_tmpl;
extern mqtt_channel_t g_mqtt;
extern osThreadId_t mqtt_task_handle;
extern const osThreadAttr_t mqtt_task_attr;

void mqtt_task(void *argument);

static inline osThreadId_t app_mqtt_start(void)
{
    return osThreadNew(mqtt_task, NULL, &mqtt_task_attr);
}

void mqtt_connection(void);
void mqtt_send_data(const char *topic, const char *message);

/**
 * @brief   设置 MQTT Broker 地址
 * @param   ip    Broker IP 地址（4 字节数组）
 * @param   port  Broker 端口号
 * @note    需在调用 mqtt_connection() 之前调用
 */
void app_mqtt_set_broker(const uint8_t ip[4], uint16_t port);
void app_mqtt_set_credentials(const char *client_id, const char *user, const char *pass);
