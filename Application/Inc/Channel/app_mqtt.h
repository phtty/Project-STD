/**
 * @file    app_mqtt.h
 * @brief   MQTT 客户端通道（Application 层）
 *
 * 连接 MQTT Broker，订阅协议登记的主题，接收数据通过 app_ccb_dispatch 写入调度框架。
 * 默认 Broker: 120.46.136.199:6000, Client ID: "CD_ZTP"
 *
 * 容器约定：
 *   - app_mqtt_ccb_t 为静态对象，base 是第一个成员（偏移 0），container_of 零开销还原；
 *   - 断线只销毁"连接"（ctx.client）并置 base.state = DOWN，控制块本身始终有效，
 *     协议侧保存的 app_ccb_t* 永不悬空，重连复用同一控制块；
 *   - 协议侧通过 app_mqtt_ccb() 取控制块（绑定用）；g_mqtt_ccb 仍对外可见，
 *     AH 协议侧据其 state 判断何时可以签到/上报。
 *
 * 当前状态：app_mqtt_start() 无调用者，链路未激活。
 */

#pragma once

#include <stdint.h>
#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief MQTT 连接状态机 */
typedef enum {
    APP_MQTT_STATE_DISCONNECTED, /**< 未连接 */
    APP_MQTT_STATE_CONNECTING,   /**< TCP + MQTT CONNECT 进行中 */
    APP_MQTT_STATE_CONNECTED,    /**< MQTT CONNACK 已接受，等待 subscribe */
    APP_MQTT_STATE_READY,        /**< subscribe 完成，可正常收发 */
} app_mqtt_state_t;

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
} app_mqtt_ctx_t;

/** @brief MQTT 通道子类（单例，app_ccb_t 为第一个成员） */
typedef struct {
    app_ccb_t base;       /**< 第一个成员：container_of 还原 */
    app_mqtt_state_t state; /**< MQTT 连接状态机 */
    app_mqtt_ctx_t ctx;     /**< 运行时上下文 */
    char topic[APP_CCB_SRC_TOPIC_MAX]; /**< 最近一次收到的主题；作为 app_ccb_dst_t.topic 缺省值 */
    /* 注：这里不再有 payload_len —— 帧长属于每条消息，寄存在通道对象上会被
       突发消息覆盖；改由探针按结尾 NUL 自行定界。 */
} app_mqtt_ccb_t;

extern const app_ccb_ops_t g_mqtt_ccb_ops;
extern app_mqtt_ccb_t g_mqtt_ccb;
extern osThreadId_t g_mqtt_task_handle;
extern const osThreadAttr_t g_mqtt_task_attr;

void app_mqtt_task(void *argument);

static inline osThreadId_t app_mqtt_start(void)
{
    return osThreadNew(app_mqtt_task, NULL, &g_mqtt_task_attr);
}

void app_mqtt_connect(void);
void app_mqtt_send(const char *topic, const void *data, uint16_t len);

/**
 * @brief 登记要订阅的主题（协议侧调用）
 *
 * 主题表由协议持有（须为静态存储期，通道长期持有指针，不拷贝）。连接建立与
 * 每次重连后由通道统一订阅；未连接时仅登记，待连接就绪再订 —— 重连由通道负责，
 * 协议不必关心时机。可多次调用追加（如认证完成后追加订阅）。
 *
 * @param topics 主题字符串数组
 * @param count  主题个数
 * @return 首个主题在订阅表中的索引（协议据此把来源主题映射回自身的命令号），
 *         失败返回 -1
 */
int32_t app_mqtt_subscribe(const char *const *topics, uint8_t count);

/**
 * @brief   设置 MQTT Broker 地址
 * @param   ip    Broker IP 地址（4 字节数组）
 * @param   port  Broker 端口号
 * @note    需在调用 app_mqtt_connect() 之前调用
 */
void app_mqtt_set_broker(const uint8_t ip[4], uint16_t port);
void app_mqtt_set_credentials(const char *client_id, const char *user, const char *pass);

/** @brief 暴露本通道控制块（协议绑定时使用）*/
app_ccb_t *app_mqtt_ccb(void);
