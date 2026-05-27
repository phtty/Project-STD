/**
 * @file    dev_mqtt.c
 * @brief   MQTT 客户端通道（Device 层）
 *
 * 连接 MQTT Broker，订阅配置主题，接收数据通过 app_channel_dispatch 写入调度框架。
 * 默认 Broker: 120.46.136.199:6000, Client ID: "CD_ZTP"
 * 当前状态：任务未创建（InitTask 中注释）
 */

#include "app_mqtt.h"
#include "app_dispatch.h"
#include "pl_net_adapt.h"
#include <string.h>

/* ---- MQTT 通道 ops ---- */
static int32_t mqtt_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    mqtt_channel_t *mqtt = container_of(ch, mqtt_channel_t, ch);
    mqtt_send_data(mqtt->topic, (char *)data);
    return len;
}

const ch_ops_t mqtt_ch_ops = { .send = mqtt_send };

/* ---- 通道元数据模板 ---- */
channel_t g_mqtt_channel_tmpl = {
    .ch_id = CH_ID_MQTT,
    .ops   = &mqtt_ch_ops,
};

mqtt_channel_t g_mqtt = {
    .ch                = { .ch_id = CH_ID_MQTT },
    .ctx.broker_ip     = {120, 46, 136, 199},
    .ctx.broker_port   = 6000,
    .ctx.client_id     = "CD_ZTP",
    .ctx.client_user   = "pxh",
    .ctx.client_pass   = "",
};

osThreadId_t mqtt_task_handle;
const osThreadAttr_t mqtt_task_attr = {
    .name       = "mqtt_task",
    .stack_size = 512 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 通道生命周期 ---- */

static void mqtt_channel_init(void)
{
    g_mqtt.ch       = g_mqtt_channel_tmpl;
    g_mqtt.ch.state = CH_STATE_UP;
    app_channel_register(CH_ID_MQTT, &g_mqtt.ch);
}

static void mqtt_channel_deinit(void)
{
    g_mqtt.ch.state = CH_STATE_DOWN;
    app_channel_register(CH_ID_MQTT, nullptr);
}

/* ---- LwIP MQTT 回调 ---- */

static void mqtt_incoming_publish_cb(void *arg, const char *topic, uint32_t tot_len)
{
    (void)arg;
    (void)tot_len;
    g_mqtt.ctx.payload_offset = 0;
    strcpy(g_mqtt.topic, topic);
}

static void mqtt_sub_request_cb(void *arg, err_t result)
{
    (void)arg;
    (void)result;
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED)
        g_mqtt.state = MQTT_ST_CONNECTED;
    else
        g_mqtt.state = MQTT_ST_DISCONNECTED;
    osSemaphoreRelease(g_mqtt.ctx.connect_sem);
}

static void mqtt_incoming_data_cb(void *arg, const uint8_t *data, uint16_t len, uint8_t flags)
{
    mqtt_ctx_t *ctx = &g_mqtt.ctx;

    if (ctx->payload_offset + len < sizeof(ctx->rcv_buf)) {
        memcpy(&ctx->rcv_buf[ctx->payload_offset], data, len);
        ctx->payload_offset += len;
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        if (ctx->payload_offset < sizeof(ctx->rcv_buf)) {
            ctx->rcv_buf[ctx->payload_offset] = '\0';
            len++;
        }

        channel_t *ch = (channel_t *)arg;
        app_channel_dispatch(ch, ctx->rcv_buf, len);
    }
}

/* ---- 连接 ---- */

void mqtt_connection(void)
{
    mqtt_ctx_t           *ctx = &g_mqtt.ctx;
    struct mqtt_connect_client_info_t mqtt_client_info;
    memset(&mqtt_client_info, 0, sizeof(mqtt_client_info));

    ip_addr_t broker_ip;
    IP4_ADDR(&broker_ip, ctx->broker_ip[0], ctx->broker_ip[1], ctx->broker_ip[2], ctx->broker_ip[3]);

    mqtt_client_info.client_id   = ctx->client_id;
    mqtt_client_info.client_user = ctx->client_user;
    mqtt_client_info.client_pass = ctx->client_pass;

    LOCK_TCPIP_CORE();
    err_t err = mqtt_client_connect((mqtt_client_t *)ctx->client, &broker_ip, ctx->broker_port,
                                    mqtt_connection_cb, NULL, &mqtt_client_info);
    UNLOCK_TCPIP_CORE();

    mqtt_set_inpub_callback((mqtt_client_t *)ctx->client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, (void *)&g_mqtt.ch);

    if (err == ERR_OK) {
        g_mqtt.state = MQTT_ST_CONNECTING;
    } else {
        g_mqtt.state = MQTT_ST_DISCONNECTED;
        mqtt_client_free((mqtt_client_t *)ctx->client);
        ctx->client = NULL;
    }
}

/* ---- 任务主循环 ---- */

void mqtt_task(void *argument)
{
    mqtt_ctx_t *ctx = &g_mqtt.ctx;

    ctx->client = mqtt_client_new();
    if (ctx->client == NULL) { osThreadExit(); return; }

    ctx->connect_sem = osSemaphoreNew(1, 0, NULL);
    mqtt_connection();

    for (;;) {
        osSemaphoreAcquire(ctx->connect_sem, osWaitForever);

        switch (g_mqtt.state) {
        case MQTT_ST_DISCONNECTED:
            mqtt_channel_deinit();
            osDelay(1000);
            ctx->client = mqtt_client_new();
            if (ctx->client) mqtt_connection();
            break;

        case MQTT_ST_CONNECTING:
            /* 连接失败：清理后重试 */
            mqtt_channel_deinit();
            osDelay(1000);
            mqtt_client_free((mqtt_client_t *)ctx->client);
            ctx->client = mqtt_client_new();
            if (ctx->client) mqtt_connection();
            break;

        case MQTT_ST_CONNECTED:
            mqtt_channel_init();
            mqtt_subscribe((mqtt_client_t *)ctx->client, "ASK/board/NULL",    0, mqtt_sub_request_cb, NULL);
            mqtt_subscribe((mqtt_client_t *)ctx->client, "ASK/display/clean", 0, mqtt_sub_request_cb, NULL);
            mqtt_subscribe((mqtt_client_t *)ctx->client, "ASK/op/restart",    0, mqtt_sub_request_cb, NULL);
            mqtt_subscribe((mqtt_client_t *)ctx->client, "ASK/op/checktime",  0, mqtt_sub_request_cb, NULL);
            g_mqtt.state = MQTT_ST_READY;
            break;

        case MQTT_ST_READY:
            /* 断连时回调设 DISCONNECTED + release sem，回到 DISCONNECTED 分支 */
            break;
        }
    }
}

void mqtt_send_data(const char *topic, const char *message)
{
    mqtt_ctx_t *ctx = &g_mqtt.ctx;

    if (ctx->client != NULL && g_mqtt.state == MQTT_ST_READY) {
        LOCK_TCPIP_CORE();
        mqtt_publish((mqtt_client_t *)ctx->client, topic, message, strlen(message), 0, 0, NULL, NULL);
        UNLOCK_TCPIP_CORE();
    }
}

void app_mqtt_set_broker(const uint8_t ip[4], uint16_t port)
{
    memcpy(g_mqtt.ctx.broker_ip, ip, 4);
    g_mqtt.ctx.broker_port = port;
}

void app_mqtt_set_credentials(const char *client_id, const char *user, const char *pass)
{
    strncpy(g_mqtt.ctx.client_id, client_id, sizeof(g_mqtt.ctx.client_id) - 1);
    strncpy(g_mqtt.ctx.client_user, user, sizeof(g_mqtt.ctx.client_user) - 1);
    strncpy(g_mqtt.ctx.client_pass, pass, sizeof(g_mqtt.ctx.client_pass) - 1);
}
