/**
 * @file    test_mqtt.c
 * @brief   MQTT 通道：订阅登记 / 凭据注入 / 连接回调 / 收发回调 / 任务重连
 *
 * MQTT 是 AH 业务的主通道，现场症状：
 *   - 同步 connect 失败没放信号量 → 主循环永远阻塞，**一次失败就再也不重连**；
 *   - 空密码当空串传给 lwIP → `ERR_VAL`，永远连不上；
 *   - 分片消息按最后一片算长度 → 报文被截断、探针切错帧；
 *   - 重连不重订 → 连上了但收不到任何命令。
 *
 * 手法：真 LwIP 的 mqtt 头文件（类型/枚举是契约）+ 脚本化 mqtt_* 行为；
 * 任务跑在 pthread 上，退出点由脚本触发 `pthread_exit()`。
 */

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_dispatch.h"
#include "app_mqtt.h"
#include "pl_net_adapt.h"

/* ================================================================
 *  断言（变体 B）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  看门狗
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似 MQTT 任务死等信号量）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* ================================================================
 *  RTOS 原语（含假时钟）
 * ================================================================ */

typedef struct {
    sem_t sem;
} _sem_t;

static uint32_t s_tick;
static int      s_delay_cnt;
static int      s_delay_1000_cnt;
static uint32_t s_delay_ms;

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                              const osSemaphoreAttr_t *attr)
{
    (void)max_count;
    (void)attr;
    _sem_t *s = (_sem_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    if (sem_init(&s->sem, 0, initial_count) != 0) {
        free(s);
        return NULL;
    }
    return (osSemaphoreId_t)s;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    _sem_t *s = (_sem_t *)semaphore_id;
    if (s == NULL) return osErrorParameter;
    if (timeout == osWaitForever) return (sem_wait(&s->sem) == 0) ? osOK : osError;
    if (timeout == 0U) return (sem_trywait(&s->sem) == 0) ? osOK : osErrorTimeout;
    return osErrorTimeout;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    _sem_t *s = (_sem_t *)semaphore_id;
    if (s == NULL) return osErrorParameter;
    return (sem_post(&s->sem) == 0) ? osOK : osError;
}

osStatus_t osDelay(uint32_t ticks)
{
    s_delay_cnt++;
    s_delay_ms = ticks;
    if (ticks == 1000U) s_delay_1000_cnt++;
    s_tick += ticks;
    /* MQTT 任务是无限重连循环：脚本已经走完时从延时里退出线程 */
    return osOK;
}

uint32_t osKernelGetTickCount(void) { return s_tick; }

void osThreadExit(void) { pthread_exit(NULL); }

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    (void)func;
    (void)argument;
    (void)attr;
    return NULL;
}

/* LwIP 的 TCPIP 核心锁：host 上没有 tcpip 线程，锁退化为空操作 */
sys_mutex_t lock_tcpip_core;
void sys_mutex_lock(sys_mutex_t *mutex) { (void)mutex; }
void sys_mutex_unlock(sys_mutex_t *mutex) { (void)mutex; }

/* ================================================================
 *  生产模块本体
 * ================================================================ */

#include "../../Application/Src/Channel/app_mqtt.c"

/* ================================================================
 *  mqtt_* 脚本替身
 * ================================================================ */

static uint8_t s_client_storage[64];
static mqtt_client_t *s_client = (mqtt_client_t *)s_client_storage;

static int  s_new_cnt;
static int  s_free_cnt;
static int  s_connect_cnt;
static int  s_sub_cnt_calls;
static int  s_publish_cnt;
static int  s_inpub_cnt;
static bool s_new_fail_once;
static bool s_connect_fail;
static bool s_connect_accept;
static bool s_disconnect_in_sub;
static bool s_exit_in_connect;
static int  s_exit_connect_at;

static err_t s_connect_err = ERR_OK;
static err_t s_sub_err     = ERR_OK;
static err_t s_pub_err     = ERR_OK;

static struct mqtt_connect_client_info_t s_info;
static uint32_t s_info_ip;
static uint16_t s_info_port;
static char     s_pub_topic[64];
static uint16_t s_pub_len;

static mqtt_connection_cb_t      s_conn_cb;
static void                     *s_conn_arg;
static mqtt_incoming_publish_cb_t s_inpub_cb;
static mqtt_incoming_data_cb_t    s_indata_cb;
static void                      *s_inpub_arg;

static int      s_disp_cnt;
static uint16_t s_disp_len[8];
static char     s_disp_topic[8][64];
static uint8_t  s_disp_data[8][1100];

static void _reset_fakes(void)
{
    s_new_cnt = s_free_cnt = s_connect_cnt = s_sub_cnt_calls = 0;
    s_publish_cnt = s_inpub_cnt = 0;
    s_new_fail_once  = false;
    s_connect_fail   = false;
    s_connect_accept = true;
    s_disconnect_in_sub = false;
    s_exit_in_connect    = false;
    s_exit_connect_at    = 0;
    s_delay_1000_cnt     = 0;
    s_connect_err = s_sub_err = s_pub_err = ERR_OK;
    memset(&s_info, 0, sizeof(s_info));
    s_info_ip = s_info_port = 0;
    memset(s_pub_topic, 0, sizeof(s_pub_topic));
    s_pub_len = 0;
    s_conn_cb = NULL;
    s_conn_arg = NULL;
    s_inpub_cb = NULL;
    s_indata_cb = NULL;
    s_inpub_arg = NULL;
    s_disp_cnt = 0;
    memset(s_disp_len, 0, sizeof(s_disp_len));
    memset(s_disp_topic, 0, sizeof(s_disp_topic));
    memset(s_disp_data, 0, sizeof(s_disp_data));
    s_tick = 0;
    s_delay_cnt = 0;
    s_delay_ms = 0;
}

mqtt_client_t *mqtt_client_new(void)
{
    s_new_cnt++;
    if (s_new_fail_once) {
        s_new_fail_once = false;
        return NULL;
    }
    return s_client;
}

void mqtt_client_free(mqtt_client_t *client)
{
    (void)client;
    s_free_cnt++;
}

err_t mqtt_client_connect(mqtt_client_t *client, const ip_addr_t *ipaddr, u16_t port,
                          mqtt_connection_cb_t cb, void *arg,
                          const struct mqtt_connect_client_info_t *client_info)
{
    (void)client;
    s_connect_cnt++;
    s_conn_cb  = cb;
    s_conn_arg = arg;
    if (client_info != NULL) {
        s_info      = *client_info;
        s_info_ip   = ip4_addr_get_u32(ip_2_ip4(ipaddr));
        s_info_port = port;
    }
    /* 真 lwIP 的 CONNACK 是**异步**回调（TCP 线程里），不会在 connect 返回前发生；
       这里只登记回调，由用例在状态机进入 CONNECTING 之后手动唤醒。 */
    if (s_exit_in_connect) pthread_exit(NULL);
    if (s_exit_connect_at > 0 && s_connect_cnt >= s_exit_connect_at) pthread_exit(NULL);
    if (s_connect_fail) return s_connect_err;
    return ERR_OK;
}

void mqtt_set_inpub_callback(mqtt_client_t *client, mqtt_incoming_publish_cb_t pub_cb,
                             mqtt_incoming_data_cb_t data_cb, void *arg)
{
    (void)client;
    s_inpub_cnt++;
    s_inpub_cb  = pub_cb;
    s_indata_cb = data_cb;
    s_inpub_arg = arg;
}

err_t mqtt_sub_unsub(mqtt_client_t *client, const char *topic, u8_t qos,
                     mqtt_request_cb_t cb, void *arg, u8_t sub)
{
    (void)client;
    (void)qos;
    (void)cb;
    (void)arg;
    (void)sub;
    (void)topic;
    s_sub_cnt_calls++;
    return s_sub_err;
}

err_t mqtt_publish(mqtt_client_t *client, const char *topic, const void *payload,
                   u16_t payload_length, u8_t qos, u8_t retain, mqtt_request_cb_t cb, void *arg)
{
    (void)client;
    (void)payload;
    (void)qos;
    (void)retain;
    (void)cb;
    (void)arg;
    s_publish_cnt++;
    if (topic != NULL) {
        strncpy(s_pub_topic, topic, sizeof(s_pub_topic) - 1);
        s_pub_topic[sizeof(s_pub_topic) - 1] = '\0';
    }
    s_pub_len = payload_length;
    return s_pub_err;
}

/* 派发记录 */
void app_ccb_dispatch(const app_ccb_t *ccb, const app_ccb_src_t *src, const uint8_t *data,
                      uint16_t len)
{
    (void)ccb;
    if (s_disp_cnt >= (int)(sizeof(s_disp_len) / sizeof(s_disp_len[0]))) return;
    s_disp_len[s_disp_cnt] = len;
    if (src != nullptr && src->topic != nullptr) {
        strncpy(s_disp_topic[s_disp_cnt], src->topic, sizeof(s_disp_topic[0]) - 1);
    }
    if (len <= sizeof(s_disp_data[0])) memcpy(s_disp_data[s_disp_cnt], data, len);
    s_disp_cnt++;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static const char *s_topic_a = "st/01/32/01/ASK/a";
static const char *s_topic_b = "st/01/32/01/ASK/b";

static void fixture_reset(void)
{
    _reset_fakes();
    s_sub_cnt = 0;
    memset(s_subs, 0, sizeof(s_subs));

    memset(&g_mqtt_ccb, 0, sizeof(g_mqtt_ccb));
    g_mqtt_ccb.base.name         = "mqtt";
    g_mqtt_ccb.base.ops          = &g_mqtt_ccb_ops;
    g_mqtt_ccb.base.state        = APP_CCB_STATE_DOWN;
    g_mqtt_ccb.state             = APP_MQTT_STATE_DISCONNECTED;
    g_mqtt_ccb.ctx.client        = NULL;
    g_mqtt_ccb.ctx.broker_ip[0]  = 120;
    g_mqtt_ccb.ctx.broker_ip[1]  = 46;
    g_mqtt_ccb.ctx.broker_ip[2]  = 136;
    g_mqtt_ccb.ctx.broker_ip[3]  = 199;
    g_mqtt_ccb.ctx.broker_port   = 6000;
    strcpy(g_mqtt_ccb.ctx.client_id, "CD_ZTP");
    strcpy(g_mqtt_ccb.ctx.client_user, "pxh");
    g_mqtt_ccb.ctx.client_pass[0] = '\0';
    g_mqtt_ccb.ctx.connect_sem    = osSemaphoreNew(1, 0, NULL);

    g_mqtt_task_handle = NULL;
}

static void *_task_body(void *arg)
{
    app_mqtt_task(arg);
    return NULL;
}

static void _wake_conn(mqtt_connection_status_t status)
{
    if (s_conn_cb != NULL) s_conn_cb(s_client, s_conn_arg, status);
}

static void _start_task(pthread_t *tid)
{
    CHECK_MSG(pthread_create(tid, NULL, _task_body, NULL) == 0, "MQTT 任务线程创建失败");
}

/* ================================================================ */

/** @brief 订阅登记表：空参数拒绝、追加返回基址、满 16 条拒绝 */
static void case_subscribe_table(void)
{
    TEST_BEGIN("订阅登记：空参数拒绝 / 追加返回基址 / 满 16 条拒绝");

    fixture_reset();

    CHECK_MSG(app_mqtt_subscribe(NULL, 1) == -1, "空主题数组必须被拒");
    CHECK_MSG(app_mqtt_subscribe(&s_topic_a, 0) == -1, "count=0 必须被拒");

    const char *two[2] = {s_topic_a, s_topic_b};
    CHECK_MSG(app_mqtt_subscribe(two, 2) == 0, "首次登记应返回基址 0");
    CHECK_MSG(s_sub_cnt == 2 && s_subs[0] == s_topic_a && s_subs[1] == s_topic_b,
              "两条主题应按序登记（%u）", (unsigned)s_sub_cnt);

    const char *three[3] = {"t3", "t4", "t5"};
    const int32_t base   = app_mqtt_subscribe(three, 3);
    CHECK_MSG(base == 2, "追加应返回基址 2，得到 %d", (int)base);

    /* 填到 16，再登记必须失败（不能越界写 s_subs） */
    const char *eleven[11] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10"};
    CHECK_MSG(app_mqtt_subscribe(eleven, 11) == 5, "继续追加应返回基址 5");
    CHECK_MSG(s_sub_cnt == 16, "登记表应恰好满 16 条（%u）", (unsigned)s_sub_cnt);
    CHECK_MSG(app_mqtt_subscribe(&s_topic_a, 1) == -1, "满 16 条后必须拒绝");
    CHECK_MSG(s_sub_cnt == 16, "被拒后不得改动表（%u）", (unsigned)s_sub_cnt);
}

/** @brief 凭据注入：有界拷贝、NULL 语义（不带字段）、broker 设置 */
static void case_credentials(void)
{
    TEST_BEGIN("凭据注入：有界拷贝 + NULL 表示不带字段");

    fixture_reset();
    const uint8_t ip[4] = {10, 0, 0, 9};
    app_mqtt_set_broker(ip, 1883);
    CHECK_MSG(g_mqtt_ccb.ctx.broker_port == 1883 && g_mqtt_ccb.ctx.broker_ip[3] == 9,
              "set_broker 应更新 IP/端口");

    app_mqtt_set_credentials("CD_TEST", "user1", "pass1");
    CHECK_MSG(strcmp(g_mqtt_ccb.ctx.client_id, "CD_TEST") == 0 &&
                  strcmp(g_mqtt_ccb.ctx.client_user, "user1") == 0 &&
                  strcmp(g_mqtt_ccb.ctx.client_pass, "pass1") == 0,
              "凭据应写入上下文");

    /* NULL 清空：三个字段都回到空串（语义是"不带该字段"） */
    app_mqtt_set_credentials(NULL, NULL, NULL);
    CHECK_MSG(g_mqtt_ccb.ctx.client_id[0] == '\0' && g_mqtt_ccb.ctx.client_user[0] == '\0' &&
                  g_mqtt_ccb.ctx.client_pass[0] == '\0',
              "NULL 应清空三个字段");

    /* 超长：有界拷贝到 31 字符 + NUL */
    char big[80];
    memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    app_mqtt_set_credentials(big, big, big);
    CHECK_MSG(strlen(g_mqtt_ccb.ctx.client_id) == sizeof(g_mqtt_ccb.ctx.client_id) - 1,
              "Client ID 应截断到 %u 字符，得到 %u", (unsigned)(sizeof(g_mqtt_ccb.ctx.client_id) - 1),
              (unsigned)strlen(g_mqtt_ccb.ctx.client_id));
    CHECK_MSG(g_mqtt_ccb.ctx.client_id[sizeof(g_mqtt_ccb.ctx.client_id) - 1] == '\0',
              "截断后必须以 NUL 结尾");
}

/** @brief connect：成功→CONNECTING + 回调注册；同步失败→DISCONNECTED 且必须放信号量 */
static void case_connect(void)
{
    TEST_BEGIN("connect：成功进 CONNECTING；同步失败置 DISCONNECTED 并放信号量");

    fixture_reset();
    g_mqtt_ccb.ctx.client = s_client;

    app_mqtt_connect();

    CHECK_MSG(s_connect_cnt == 1, "应提交一次连接（%d）", s_connect_cnt);
    CHECK_MSG(s_info_port == 6000, "Broker 端口应为 6000，得到 %u", (unsigned)s_info_port);
    ip4_addr_t expect_ip;
    IP4_ADDR(&expect_ip, 120, 46, 136, 199);
    CHECK_MSG(s_info_ip == ip4_addr_get_u32(&expect_ip), "Broker IP 应透传 120.46.136.199（%08X）",
              (unsigned)s_info_ip);
    CHECK_MSG(s_inpub_cnt == 1 && s_inpub_cb != NULL && s_indata_cb != NULL,
              "必须注册 incoming publish/data 回调");
    CHECK_MSG(s_inpub_arg == (void *)&g_mqtt_ccb.base, "回调上下文应是通道基类指针");
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_CONNECTING,
              "提交成功后状态应为 CONNECTING，得到 %d", (int)g_mqtt_ccb.state);
    CHECK_MSG(osSemaphoreAcquire(g_mqtt_ccb.ctx.connect_sem, 0) == osErrorTimeout,
              "CONNACK 还没来，信号量不应被提前释放");

    _wake_conn(MQTT_CONNECT_ACCEPTED);
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_CONNECTED,
              "ACCEPTED 回调应置 CONNECTED，得到 %d", (int)g_mqtt_ccb.state);
    CHECK_MSG(osSemaphoreAcquire(g_mqtt_ccb.ctx.connect_sem, 0) == osOK,
              "连接回调必须放信号量唤醒主循环");
    CHECK_MSG(s_info.client_id != NULL && strcmp(s_info.client_id, "CD_ZTP") == 0,
              "Client ID 应透传（%s）", s_info.client_id ? s_info.client_id : "(null)");
    CHECK_MSG(s_info.client_user != NULL && strcmp(s_info.client_user, "pxh") == 0,
              "用户名应透传");
    CHECK_MSG(s_info.client_pass == NULL, "空密码必须传 NULL（lwIP 对空串返回 ERR_VAL）");

    /* 同步失败：不会再走连接回调，必须自己放信号量，否则一次失败就永不重连 */
    fixture_reset();
    g_mqtt_ccb.ctx.client = s_client;
    s_connect_fail        = true;
    s_connect_err         = ERR_CONN;

    app_mqtt_connect();
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_DISCONNECTED,
              "同步失败必须置 DISCONNECTED，得到 %d", (int)g_mqtt_ccb.state);
    CHECK_MSG(osSemaphoreAcquire(g_mqtt_ccb.ctx.connect_sem, 0) == osOK,
              "同步失败也必须放信号量（否则永不重连）");

    /* 有密码时必须传指针而不是 NULL */
    fixture_reset();
    g_mqtt_ccb.ctx.client = s_client;
    app_mqtt_set_credentials("CD", "u", "p");
    app_mqtt_connect();
    CHECK_MSG(s_info.client_pass != NULL && strcmp(s_info.client_pass, "p") == 0,
              "非空密码必须透传");
}

/** @brief 发送：DOWN 丢弃；UP+READY 才发布；dst topic 优先于本帧来源主题 */
static void case_send(void)
{
    TEST_BEGIN("发送：DOWN 丢弃；READY 才 publish；dst->topic 优先");

    fixture_reset();
    uint8_t data[4] = {1, 2, 3, 4};

    g_mqtt_ccb.ctx.client = s_client;
    g_mqtt_ccb.base.state = APP_CCB_STATE_DOWN;
    CHECK_MSG(g_mqtt_ccb.base.ops->send(&g_mqtt_ccb.base, NULL, data, sizeof(data)) == -1,
              "DOWN 必须丢弃");
    CHECK_MSG(s_publish_cnt == 0, "DOWN 不得 publish（%d）", s_publish_cnt);

    g_mqtt_ccb.base.state = APP_CCB_STATE_UP;
    g_mqtt_ccb.state      = APP_MQTT_STATE_READY;
    strcpy(g_mqtt_ccb.topic, "last/topic");
    CHECK_MSG(g_mqtt_ccb.base.ops->send(&g_mqtt_ccb.base, NULL, data, sizeof(data)) == 4,
              "READY 时 send 应返回字节数");
    CHECK_MSG(s_publish_cnt == 1 && strcmp(s_pub_topic, "last/topic") == 0 && s_pub_len == 4,
              "无 dst 时应发到本帧来源主题（%s/%u）", s_pub_topic, (unsigned)s_pub_len);

    const app_ccb_dst_t dst = {.topic = "dst/topic"};
    CHECK_MSG(g_mqtt_ccb.base.ops->send(&g_mqtt_ccb.base, &dst, data, sizeof(data)) == 4,
              "带 dst 也应返回字节数");
    CHECK_MSG(strcmp(s_pub_topic, "dst/topic") == 0, "dst->topic 应优先（%s）", s_pub_topic);

    /* 非 READY：不发布，但 send 仍返回长度（通道不阻塞协议） */
    const int before      = s_publish_cnt;
    g_mqtt_ccb.state      = APP_MQTT_STATE_CONNECTED;
    CHECK_MSG(g_mqtt_ccb.base.ops->send(&g_mqtt_ccb.base, NULL, data, sizeof(data)) == 4,
              "非 READY 时 send 仍应返回长度");
    CHECK_MSG(s_publish_cnt == before, "非 READY 不得 publish（%d）", s_publish_cnt);
}

/** @brief 接收：主题有界拷贝、分片累计、LAST 才派发、偏移复位、溢出夹取 */
static void case_incoming(void)
{
    TEST_BEGIN("接收：主题截断 / 分片累计 / LAST 派发 / 偏移复位 / 溢出夹取");

    fixture_reset();
    static const uint8_t f1[] = "abc";
    static const uint8_t f2[] = "def";

    _mqtt_incoming_publish_fn(NULL, "topic/1", 6);
    CHECK_MSG(strcmp(g_mqtt_ccb.topic, "topic/1") == 0, "主题应写入通道（%s）",
              g_mqtt_ccb.topic);
    CHECK_MSG(g_mqtt_ccb.ctx.payload_offset == 0, "新消息必须复位偏移（%u）",
              (unsigned)g_mqtt_ccb.ctx.payload_offset);

    _mqtt_incoming_data_fn(&g_mqtt_ccb.base, f1, sizeof(f1) - 1, 0);
    CHECK_MSG(s_disp_cnt == 0, "非 LAST 分片不得派发（%d）", s_disp_cnt);
    _mqtt_incoming_data_fn(&g_mqtt_ccb.base, f2, sizeof(f2) - 1, MQTT_DATA_FLAG_LAST);

    CHECK_MSG(s_disp_cnt == 1, "LAST 应派发一次（%d）", s_disp_cnt);
    CHECK_MSG(s_disp_len[0] == 7, "派发长度应是累计 6 + 结尾 NUL = 7，得到 %u",
              (unsigned)s_disp_len[0]);
    CHECK_MSG(memcmp(s_disp_data[0], "abcdef\0", 7) == 0, "派发内容应为 abcdef + NUL");
    CHECK_MSG(strcmp(s_disp_topic[0], "topic/1") == 0, "来源主题应随通知给出（%s）",
              s_disp_topic[0]);

    /* 第二条消息：偏移必须复位，不能接在上一条后面 */
    _mqtt_incoming_publish_fn(NULL, "topic/2", 3);
    _mqtt_incoming_data_fn(&g_mqtt_ccb.base, (const uint8_t *)"xy", 2, MQTT_DATA_FLAG_LAST);
    CHECK_MSG(s_disp_cnt == 2 && s_disp_len[1] == 3, "第二条消息应独立成帧（%u）",
              (unsigned)s_disp_len[1]);
    CHECK_MSG(memcmp(s_disp_data[1], "xy\0", 3) == 0, "第二条内容应为 xy + NUL");

    /* 超长主题：必须有界拷贝并以 NUL 结尾 */
    char big[200];
    memset(big, 'T', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    _mqtt_incoming_publish_fn(NULL, big, 0);
    CHECK_MSG(strlen(g_mqtt_ccb.topic) == sizeof(g_mqtt_ccb.topic) - 1,
              "超长主题应截断到 %u 字符（%u）", (unsigned)(sizeof(g_mqtt_ccb.topic) - 1),
              (unsigned)strlen(g_mqtt_ccb.topic));

    /* 溢出：累计超过 rcv_buf 时夹到 sizeof-1，且仍以 NUL 结尾 */
    _mqtt_incoming_publish_fn(NULL, "big", 0);
    static uint8_t chunk[600];
    memset(chunk, 'A', sizeof(chunk));
    _mqtt_incoming_data_fn(&g_mqtt_ccb.base, chunk, sizeof(chunk), 0);
    _mqtt_incoming_data_fn(&g_mqtt_ccb.base, chunk, sizeof(chunk), MQTT_DATA_FLAG_LAST);
    CHECK_MSG(s_disp_cnt == 3, "溢出也应派发（%d）", s_disp_cnt);
    CHECK_MSG(s_disp_len[2] == 601, "放不下的整片被丢弃，长度应停在 600 + NUL = 601（%u）",
              (unsigned)s_disp_len[2]);
    CHECK_MSG(s_disp_data[2][600] == '\0', "派发内容末字节必须是 NUL");
}

/** @brief 连接回调：ACCEPTED → CONNECTED；其余 → DISCONNECTED；都要放信号量 */
static void case_connection_cb(void)
{
    TEST_BEGIN("连接回调：ACCEPTED→CONNECTED，其余→DISCONNECTED，都放信号量");

    fixture_reset();
    _mqtt_connection_fn(s_client, NULL, MQTT_CONNECT_ACCEPTED);
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_CONNECTED, "ACCEPTED 应置 CONNECTED（%d）",
              (int)g_mqtt_ccb.state);
    CHECK_MSG(osSemaphoreAcquire(g_mqtt_ccb.ctx.connect_sem, 0) == osOK, "ACCEPTED 应放信号量");

    _mqtt_connection_fn(s_client, NULL, MQTT_CONNECT_REFUSED_PROTOCOL_VERSION);
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_DISCONNECTED, "拒绝应置 DISCONNECTED（%d）",
              (int)g_mqtt_ccb.state);
    CHECK_MSG(osSemaphoreAcquire(g_mqtt_ccb.ctx.connect_sem, 0) == osOK, "拒绝也应放信号量");
}

/** @brief 任务：连接成功 → 通道 UP + 按登记表逐条订阅 */
static void case_task_connected(void)
{
    TEST_BEGIN("任务：ACCEPTED → 通道 UP + 逐条订阅（含空表）");

    fixture_reset();
    const char *topics[2] = {s_topic_a, s_topic_b};
    CHECK_MSG(app_mqtt_subscribe(topics, 2) == 0, "预登记 2 条主题");

    pthread_t tid;
    _start_task(&tid);

    /* 等任务提交连接，再异步给 CONNACK（真机就是这样） */
    for (int i = 0; i < 2000 && s_connect_cnt == 0; i++) {
        struct timespec wait = {.tv_nsec = 1000000L};
        nanosleep(&wait, NULL);
    }
    _wake_conn(MQTT_CONNECT_ACCEPTED);

    /* 等任务把两件事做完：通道置 UP、按登记表订阅两条、状态推到 READY。 */
    for (int i = 0; i < 2000 && g_mqtt_ccb.state != APP_MQTT_STATE_READY; i++) {
        struct timespec ts = {.tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }
    CHECK_MSG(g_mqtt_ccb.base.state == APP_CCB_STATE_UP, "连接就绪后通道应置 UP（%d）",
              (int)g_mqtt_ccb.base.state);
    CHECK_MSG(g_mqtt_ccb.state == APP_MQTT_STATE_READY, "订阅完成后状态应为 READY（%d）",
              (int)g_mqtt_ccb.state);
    CHECK_MSG(s_sub_cnt_calls == 2, "应按登记表订阅 2 条（%d）", s_sub_cnt_calls);

    /* 收尾：任务阻塞在信号量上。触发断连让它走重连分支，下一次 connect 时
       由脚本从任务线程退出（避免无限重连）。 */
    s_exit_in_connect = true;
    _wake_conn(MQTT_CONNECT_REFUSED_PROTOCOL_VERSION);
    pthread_join(tid, NULL);
}

/** @brief 任务：同步连接失败也必须重试（free + 1s + 重建），否则一次失败就断线到死 */
static void case_task_failure(void)
{
    TEST_BEGIN("任务：同步连接失败 → free + 1s + 重建（证明信号量纪律）");

    fixture_reset();
    s_connect_fail      = true;
    s_connect_err       = ERR_CONN;
    s_exit_connect_at   = 3; /* 第 3 次连接时从任务线程退出（否则无限重连） */

    pthread_t tid;
    _start_task(&tid);
    pthread_join(tid, NULL);

    CHECK_MSG(s_free_cnt >= 1, "失败后应释放旧客户端（%d）", s_free_cnt);
    CHECK_MSG(s_delay_1000_cnt >= 1, "应退避 1000ms（%d 次）", s_delay_1000_cnt);
    CHECK_MSG(s_connect_cnt >= 2, "应重建客户端并再连（connect=%d）", s_connect_cnt);
}

/* ================================================================ */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36mMQTT 通道（订阅/凭据/连接回调/收发/任务重连）\033[0m\n");

    case_subscribe_table();
    case_credentials();
    case_connect();
    case_send();
    case_incoming();
    case_connection_cb();
    case_task_connected();
    case_task_failure();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
