#include "mqtt_app.h"

#include "lwip.h"
#include "lwip/sys.h"
#include "lwip/apps/mqtt.h"

// Broker 服务器 IP
#define BROKER_IP_ADDR "192.168.114.100"
#define BROKER_PORT    1883

static mqtt_client_t *mqtt_client;                         // MQTT 客户端控制块
static struct mqtt_connect_client_info_t mqtt_client_info; // 连接信息
static uint32_t current_payload_offset = 0;                // mqtt数据接收缓冲区偏移计数
static uint8_t mqtt_rcv_buf[1024];                         // mqtt数据接收缓冲区
volatile mqtt_StateMachine mqtt_state = no_connect;        // mqtt连接状态状态机
osSemaphoreId_t mqttConnSemHandle;                         // 断开连接标志信号量

osThreadId_t mqttManageTaskHandle;
const osThreadAttr_t mqttManageTask_attributes = {
    .name       = "mqttManageTask",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

void mqttManageTask(void *argument)
{
    for (;;) {
        // 等待网络接口就绪事件标志
        if (!(netif_is_up(netif_default) && netif_is_link_up(netif_default))) {
            osDelay(100);
            continue;
        }

        if (mqtt_state == connected) {
            // 连接正常，阻塞等待断开事件
            osSemaphoreAcquire(mqttConnSemHandle, osWaitForever);

            // char *topic  = "test/topic";
            // char msg[64] = {0};
            // snprintf(msg, sizeof(msg), "mqtt publish test %d", mqtt_count);
            // mqtt_send_data(topic, msg);

        } else {
            mqtt_connection();
            // 等待连接结果，10s超时
            osSemaphoreAcquire(mqttConnSemHandle, 10000);
            if (mqtt_state != connected) {
                // 连接失败，1s后重连
                osDelay(1000);
            }
        }
    }
}

/**
 * @brief 处理 Payload 数据回调 (可能分多次调用)
 *
 * @param arg
 * @param data 数据指针
 * @param len 本次长度
 * @param flags MQTT_DATA_FLAG_LAST 表示这是最后一片
 */
static void mqtt_incoming_data_cb(void *arg, const uint8_t *data, uint16_t len, uint8_t flags)
{
    // 防止缓冲区溢出
    if (current_payload_offset + len < sizeof(mqtt_rcv_buf)) {
        memcpy(&mqtt_rcv_buf[current_payload_offset], data, len);
        current_payload_offset += len;
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        // 加\0，方便操作字符串
        if (current_payload_offset < sizeof(mqtt_rcv_buf)) {
            mqtt_rcv_buf[current_payload_offset] = '\0';
            len++;
        }
        // 将数据放入环形缓冲区
        BSP_RB_PutByte_Bulk(&xProtocal_RB, (uint8_t *)data, len);
    }
}

/**
 * @brief 收到publish帧的回调
 *
 * @param arg
 * @param topic
 * @param tot_len
 */
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    // 重置偏移量，准备接收新包
    current_payload_offset = 0;
}

// 订阅成功的回调
static void mqtt_sub_request_cb(void *arg, err_t result)
{
    printf("subscribe test successfully");
}

/**
 * @brief 连接回调
 *
 * @param client
 * @param arg
 * @param status
 */
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    err_t err;

    if (status == MQTT_CONNECT_ACCEPTED) {
        mqtt_state = connected;
        // 连上后，订阅一个主题test,QoS设置为1, 回调函数 mqtt_sub_request_cb
        err = mqtt_subscribe(client, "test/topic", 1, mqtt_sub_request_cb, NULL);
        if (err != ERR_OK) {
            return;
        }

    } else {
        mqtt_state = no_connect;
    }

    osSemaphoreRelease(mqttConnSemHandle); // 释放信号量通知连接管理任务
}

void mqtt_connection(void)
{
    ip_addr_t broker_ip;
    err_t err;

    if (mqtt_state == connecting)
        return;

    // 加互斥锁防止和lwip的任务资源竞争
    LOCK_TCPIP_CORE();

    if (mqtt_client != NULL) {
        mqtt_disconnect(mqtt_client);
        mqtt_client_free(mqtt_client);
        mqtt_client = NULL;
    }

    mqtt_client = mqtt_client_new();
    if (mqtt_client == NULL) {
        UNLOCK_TCPIP_CORE(); // 解锁
        return;
    }

    ipaddr_aton(BROKER_IP_ADDR, &broker_ip);

    memset(&mqtt_client_info, 0, sizeof(mqtt_client_info));
    mqtt_client_info.client_id  = "STM32F407";
    mqtt_client_info.keep_alive = 60; // 60s 心跳
    // mqtt_client_info.client_user = "user"; // 用户名和密码
    // mqtt_client_info.client_pass = "pass";

    // 注册接收回调
    mqtt_set_inpub_callback(mqtt_client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, NULL);

    err = mqtt_client_connect(mqtt_client, &broker_ip, BROKER_PORT, mqtt_connection_cb, NULL, &mqtt_client_info);
    if (err == ERR_OK) {

        mqtt_state = connecting;

    } else { // 连接发起失败，释放资源
        mqtt_state = no_connect;
        mqtt_client_free(mqtt_client);
        mqtt_client = NULL;
    }

    UNLOCK_TCPIP_CORE(); // 解锁
}

/**
 * @brief 发送确认回调，会根据QoS有不同的调用机制
 *
 * @param arg
 * @param result
 */
static void mqtt_pub_request_cb(void *arg, err_t result)
{
}

void mqtt_send_data(const char *topic, const char *message)
{
    err_t err;

    // 加互斥锁防止和lwip的任务资源竞争
    LOCK_TCPIP_CORE();

    // qos 1, retain 0
    if (mqtt_client != NULL && mqtt_client_is_connected(mqtt_client))
        err = mqtt_publish(mqtt_client, topic, message, strlen(message), 1, 0, mqtt_pub_request_cb, NULL);

    UNLOCK_TCPIP_CORE(); // 解锁

    if (err != ERR_OK) {
        ;
    }
}
