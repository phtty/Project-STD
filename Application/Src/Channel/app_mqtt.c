/**
 * @file    app_mqtt.c
 * @brief   MQTT 客户端通道（Application 层）
 *
 * 连接 MQTT Broker，订阅配置主题，接收数据通过 app_ccb_dispatch 写入调度框架。
 * Broker/Client ID/凭据由调用方注入（AH 用 `ah_cfg` 派生）；未注入时保留内置默认值。
 * 启动方式：`app_boot` 的 `_init_task` 调 `app_mqtt_start()`；连接/重连由本通道任务负责。
 */

#include "app_mqtt.h"
#include "app_dispatch.h"
#include "pl_net_adapt.h"
#include <stdio.h>
#include <string.h>

/* ---- MQTT 通道 ops ---- */
static int32_t _mqtt_send(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    app_mqtt_ccb_t *mqtt = container_of(ccb, app_mqtt_ccb_t, base);
    if (mqtt->base.state != APP_CCB_STATE_UP) return -1;

    /* 目的地：协议显式给的 topic 优先，否则回落到本帧来源主题 */
    const char *topic = (dst != nullptr && dst->topic != nullptr) ? dst->topic : mqtt->topic;

    app_mqtt_send(topic, data, len); /* len 透传，不再内部 strlen */
    return (int32_t)len;
}

const app_ccb_ops_t g_mqtt_ccb_ops = { .send = _mqtt_send };

/* ---- 通道控制块（静态，协议绑定期间即可用） ---- */
app_mqtt_ccb_t g_mqtt_ccb = {
    .base = { .name = "mqtt", .ops = &g_mqtt_ccb_ops },
    .ctx.broker_ip     = {120, 46, 136, 199},
    .ctx.broker_port   = 6000,
    .ctx.client_id     = "CD_ZTP",
    .ctx.client_user   = "pxh",
    .ctx.client_pass   = "",
};

app_ccb_t *app_mqtt_ccb(void)
{
    return &g_mqtt_ccb.base;
}

osThreadId_t g_mqtt_task_handle;
const osThreadAttr_t g_mqtt_task_attr = {
    .name       = "app_mqtt_task",
    .stack_size = 384 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 订阅登记 ----
 * 订阅哪些主题是协议知识，通道只知道"如何订一个主题"。协议登记一次，
 * 通道在每次连接就绪时施加（含重连）。 */
#define MQTT_SUB_MAX (16U)

static const char *s_subs[MQTT_SUB_MAX];
static uint8_t     s_sub_cnt;

int32_t app_mqtt_subscribe(const char *const *topics, uint8_t count)
{
    if (topics == nullptr || count == 0) return -1;
    if (s_sub_cnt + count > MQTT_SUB_MAX) return -1;

    int32_t base = (int32_t)s_sub_cnt;
    for (uint8_t i = 0; i < count; i++)
        s_subs[s_sub_cnt++] = topics[i];
    return base;
}

/* ---- 通道生命周期 ---- */

static void _mqtt_channel_init(void)
{
    g_mqtt_ccb.base.state = APP_CCB_STATE_UP;
}

static void _mqtt_channel_deinit(void)
{
    g_mqtt_ccb.base.state = APP_CCB_STATE_DOWN;
}

/* ---- LwIP MQTT 回调 ---- */

static void _mqtt_incoming_publish_fn(void *arg, const char *topic, uint32_t tot_len)
{
    (void)arg;
    (void)tot_len;
    g_mqtt_ccb.ctx.payload_offset = 0;

    /* 有界拷贝：topic 长度由 broker 控制，直接 strcpy 会写穿 topic[] */
    strncpy(g_mqtt_ccb.topic, topic, sizeof(g_mqtt_ccb.topic) - 1);
    g_mqtt_ccb.topic[sizeof(g_mqtt_ccb.topic) - 1] = '\0';
}

static void _mqtt_sub_request_fn(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK)
        printf("[mqtt] 订阅失败（result=%d），重连后会重订\n", (int)result);
}

static void _mqtt_connection_fn(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED)
        g_mqtt_ccb.state = APP_MQTT_STATE_CONNECTED;
    else
        g_mqtt_ccb.state = APP_MQTT_STATE_DISCONNECTED;
    osSemaphoreRelease(g_mqtt_ccb.ctx.connect_sem);
}

static void _mqtt_incoming_data_fn(void *arg, const uint8_t *data, uint16_t len, uint8_t flags)
{
    app_mqtt_ctx_t *ctx = &g_mqtt_ccb.ctx;

    if (ctx->payload_offset + len < sizeof(ctx->rcv_buf)) {
        memcpy(&ctx->rcv_buf[ctx->payload_offset], data, len);
        ctx->payload_offset += len;
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        /* 以 NUL 结尾（协议侧按字符串处理），长度含该 NUL。
           长度按累计的 payload_offset 算，不能用本回调最后一次分片的 len ——
           分片投递时后者只是尾巴，会让整条消息被截断。 */
        uint16_t total = ctx->payload_offset;
        if (total >= sizeof(ctx->rcv_buf)) total = sizeof(ctx->rcv_buf) - 1;
        ctx->rcv_buf[total] = '\0';

        /* 来源随通知交给框架：探针据此分类。
           帧长不必由通道给出 —— 探针按结尾 NUL 自行定界，连续两条消息
           同处缓冲区时也能各自成帧。
           src 只需在本次调用期间有效：框架把主题内容拷进自己的通知元素。 */
        const app_ccb_src_t src = {.topic = g_mqtt_ccb.topic};
        app_ccb_t *ccb      = (app_ccb_t *)arg;
        app_ccb_dispatch(ccb, &src, ctx->rcv_buf, (uint16_t)(total + 1));
    }
}

/* ---- 连接 ---- */

void app_mqtt_connect(void)
{
    app_mqtt_ctx_t           *ctx = &g_mqtt_ccb.ctx;
    struct mqtt_connect_client_info_t mqtt_client_info;
    memset(&mqtt_client_info, 0, sizeof(mqtt_client_info));

    ip_addr_t broker_ip;
    IP4_ADDR(&broker_ip, ctx->broker_ip[0], ctx->broker_ip[1], ctx->broker_ip[2], ctx->broker_ip[3]);

    /* lwIP 对“非 NULL 但长度 0”的 user/pass 直接返回 ERR_VAL；空串必须传 NULL，
       表示不带该字段（broker 不要求密码时就是这样）。 */
    mqtt_client_info.client_id   = ctx->client_id;
    mqtt_client_info.client_user = ctx->client_user[0] != '\0' ? ctx->client_user : nullptr;
    mqtt_client_info.client_pass = ctx->client_pass[0] != '\0' ? ctx->client_pass : nullptr;

    LOCK_TCPIP_CORE();
    err_t err = mqtt_client_connect((mqtt_client_t *)ctx->client, &broker_ip, ctx->broker_port,
                                    _mqtt_connection_fn, NULL, &mqtt_client_info);
    UNLOCK_TCPIP_CORE();

    /* 回调上下文取基类指针：接收回调据此直接派发，不必再回查全局 */
    mqtt_set_inpub_callback((mqtt_client_t *)ctx->client, _mqtt_incoming_publish_fn, _mqtt_incoming_data_fn, (void *)&g_mqtt_ccb.base);

    if (err == ERR_OK) {
        g_mqtt_ccb.state = APP_MQTT_STATE_CONNECTING;
    } else {
        /* 同步失败不会再走连接回调；必须自己放信号量，否则主循环永远阻塞在
           osSemaphoreAcquire(osWaitForever)，一次失败就再也不重连。客户端由主循环释放。 */
        g_mqtt_ccb.state = APP_MQTT_STATE_DISCONNECTED;
        osSemaphoreRelease(ctx->connect_sem);
    }
}

/* ---- 任务主循环 ---- */

void app_mqtt_task(void *argument)
{
    (void)argument; /* 单例通道：上下文取自静态控制块，不用任务参数 */
    app_mqtt_ctx_t *ctx = &g_mqtt_ccb.ctx;

    ctx->connect_sem = osSemaphoreNew(1, 0, NULL);
    if (ctx->connect_sem == NULL) {
        osThreadExit();
        return;
    }

    while ((ctx->client = mqtt_client_new()) == nullptr)
        osDelay(1000);

    app_mqtt_connect();

    for (;;) {
        osSemaphoreAcquire(ctx->connect_sem, osWaitForever);

        switch (g_mqtt_ccb.state) {
        case APP_MQTT_STATE_CONNECTED:
            _mqtt_channel_init();
            /* 协议登记、通道施加：每次连接（含重连）统一重订，主题表在协议侧 */
            for (uint8_t i = 0; i < s_sub_cnt; i++) {
                err_t err = mqtt_subscribe((mqtt_client_t *)ctx->client, s_subs[i], 0,
                                           _mqtt_sub_request_fn, nullptr);
                if (err != ERR_OK)
                    printf("[mqtt] 订阅 %s 提交失败（err=%d）\n", s_subs[i], (int)err);
            }
            g_mqtt_ccb.state = APP_MQTT_STATE_READY;
            break;

        case APP_MQTT_STATE_DISCONNECTED:
        case APP_MQTT_STATE_CONNECTING:
        default:
            /* 连接失败/掉线：释放旧客户端，退避 1 s 后重建再连 */
            _mqtt_channel_deinit();
            if (ctx->client != nullptr) {
                mqtt_client_free((mqtt_client_t *)ctx->client);
                ctx->client = nullptr;
            }
            osDelay(1000);
            while ((ctx->client = mqtt_client_new()) == nullptr)
                osDelay(1000);
            app_mqtt_connect();
            break;

        case APP_MQTT_STATE_READY:
            /* 断连时回调设 DISCONNECTED + release sem，回到重连分支 */
            break;
        }
    }
}

void app_mqtt_send(const char *topic, const void *data, uint16_t len)
{
    app_mqtt_ctx_t *ctx = &g_mqtt_ccb.ctx;

    if (ctx->client != nullptr && g_mqtt_ccb.state == APP_MQTT_STATE_READY) {
        LOCK_TCPIP_CORE();
        err_t err = mqtt_publish((mqtt_client_t *)ctx->client, topic, data, len, 0, 0, nullptr,
                                 nullptr);
        UNLOCK_TCPIP_CORE();
        if (err != ERR_OK)
            printf("[mqtt] 发布 %s 失败（err=%d）\n", topic, (int)err);
    }
}

void app_mqtt_set_broker(const uint8_t ip[4], uint16_t port)
{
    memcpy(g_mqtt_ccb.ctx.broker_ip, ip, 4);
    g_mqtt_ccb.ctx.broker_port = port;
}

void app_mqtt_set_credentials(const char *client_id, const char *user, const char *pass)
{
    /* user/pass 允许 NULL：语义是“不带该字段”，不是空字符串 */
    g_mqtt_ccb.ctx.client_id[0]   = '\0';
    g_mqtt_ccb.ctx.client_user[0] = '\0';
    g_mqtt_ccb.ctx.client_pass[0] = '\0';

    if (client_id != nullptr)
        strncpy(g_mqtt_ccb.ctx.client_id, client_id, sizeof(g_mqtt_ccb.ctx.client_id) - 1);
    if (user != nullptr)
        strncpy(g_mqtt_ccb.ctx.client_user, user, sizeof(g_mqtt_ccb.ctx.client_user) - 1);
    if (pass != nullptr)
        strncpy(g_mqtt_ccb.ctx.client_pass, pass, sizeof(g_mqtt_ccb.ctx.client_pass) - 1);
}
