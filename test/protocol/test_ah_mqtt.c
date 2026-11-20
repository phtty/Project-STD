/**
 * @file    test_ah_mqtt.c
 * @brief   AH/MQTT 上电装配：主题派生、凭据注入、订阅登记、探针分类
 *
 * **为什么需要这个测试**：MQTT 这条链路以前“能编译、从没跑通”，根因全是
 * 身份字符串与注入时机：主题只存后缀、探针却比完整主题；密码空串传给 lwIP
 * 直接 ERR_VAL；订阅主题与分类主题各写一份。现在主题只在 sw_post 构建一次，
 * 这里把“配置 → 主题/ClientID/凭据 → 订阅表”整条装配线逐字符钉住。
 *
 * 手法：用例 TU-include 生产 TU `app_ah_mqtt.c`（装配函数是 static），
 * 通道/任务/时钟用记录型桩；探针走真实 ring buffer。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_ah.h"
#include "app_ah_cfg.h"
#include "app_ah_mqtt.h"
#include "app_ah_mqtt_cmd.h"
#include "app_dispatch.h"
#include "app_mqtt.h"
#include "dev_display.h"
#include "pl_rtc.h"
#include "ring_buffer.h"

/* 生产实现 TU-include：`_ah_mqtt_module_init`、`_build_topics` 等是 static */
#include "../../Application/Src/AH/app_ah_mqtt.c"

/* ---- 断言 ---- */
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
 *  桩：配置 / MQTT 通道 / 任务 / 时钟 / 显示 / 分发绑定
 * ================================================================ */

static app_ah_cfg_t s_cfg;

void app_ah_cfg_get(app_ah_cfg_t *out)
{
    if (out != nullptr)
        *out = s_cfg;
}

static int      s_broker_calls;
static uint8_t  s_broker_ip[4];
static uint16_t s_broker_port;

void app_mqtt_set_broker(const uint8_t ip[4], uint16_t port)
{
    s_broker_calls++;
    memcpy(s_broker_ip, ip, 4);
    s_broker_port = port;
}

static int  s_cred_calls;
static char s_cred_id[APP_AH_CLIENT_ID_MAX];
static char s_cred_user[32];
static bool s_cred_user_null;
static bool s_cred_pass_null;

void app_mqtt_set_credentials(const char *client_id, const char *user, const char *pass)
{
    s_cred_calls++;
    snprintf(s_cred_id, sizeof(s_cred_id), "%s", client_id != nullptr ? client_id : "");
    s_cred_user_null = (user == nullptr);
    s_cred_pass_null = (pass == nullptr);
    snprintf(s_cred_user, sizeof(s_cred_user), "%s", user != nullptr ? user : "");
}

static int         s_sub_calls;
static uint8_t     s_sub_count;
static const char *s_sub_ptrs[AH_MQTT_CMD_COUNT];
static int32_t     s_sub_rc;

int32_t app_mqtt_subscribe(const char *const *topics, uint8_t count)
{
    s_sub_calls++;
    s_sub_count = count;
    for (uint8_t i = 0; i < count && i < AH_MQTT_CMD_COUNT; i++)
        s_sub_ptrs[i] = topics[i];
    return s_sub_rc;
}

app_mqtt_ccb_t g_mqtt_ccb;

app_ccb_t *app_mqtt_ccb(void)
{
    return &g_mqtt_ccb.base;
}

void app_mqtt_send(const char *topic, const void *data, uint16_t len)
{
    (void)topic;
    (void)data;
    (void)len;
}

static int  s_task_calls;
static int  s_fake_task;
static bool s_inited;

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    s_task_calls++;
    return (osThreadId_t)&s_fake_task;
}

static int s_bind_calls;

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
    s_bind_calls++;
}

static uint32_t s_rtc_ts;

pl_rtc_handle_t pl_rtc_get_handle(void)
{
    return nullptr;
}

uint32_t pl_rtc_get_timestamp(pl_rtc_handle_t h)
{
    (void)h;
    return s_rtc_ts;
}

static dev_display_t s_display;

dev_display_t *dev_display_get(void)
{
    return &s_display;
}

/* 命令表由生产 cmd.c 定义；本套件不测命令处理，只提供后缀来源 */
const app_ah_mqtt_cmd_entry_t g_ah_mqtt_cmd_table[AH_MQTT_CMD_COUNT] = {
    {"ASK/board/NULL", nullptr},
    {"ASK/display/clean", nullptr},
    {"ASK/op/restart", nullptr},
    {"ASK/op/checktime", nullptr},
};

/* ================================================================
 *  夹具
 * ================================================================ */

static app_ah_cfg_t _cfg_make(void)
{
    app_ah_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    const uint8_t lane[5] = {0x52U, 0x01U, 0x13U, 0x14U, 0x01U};
    memcpy(cfg.lane_hex, lane, sizeof(lane));
    cfg.device_id  = (uint8_t)AH_DEV_LED_BOARD;
    cfg.device_no  = 0x01U;
    cfg.mqtt_ip[0] = 120U;
    cfg.mqtt_ip[1] = 46U;
    cfg.mqtt_ip[2] = 136U;
    cfg.mqtt_ip[3] = 199U;
    cfg.mqtt_port  = 6000U;
    memcpy(cfg.mqtt_user, "pxh", 3); /* 密码保持空：必须注入 nullptr */
    return cfg;
}

static void _reset(void)
{
    s_cfg            = _cfg_make();
    s_broker_calls   = 0;
    s_cred_calls     = 0;
    s_sub_calls      = 0;
    s_sub_count      = 0;
    s_sub_rc         = 0;
    s_task_calls     = 0;
    s_bind_calls     = 0;
    s_rtc_ts         = 1741183200U;
    s_display.light_level = 1U;
    memset(s_broker_ip, 0, sizeof(s_broker_ip));
    s_broker_port = 0U;
    memset(s_cred_id, 0, sizeof(s_cred_id));
    memset(s_cred_user, 0, sizeof(s_cred_user));
    s_cred_user_null = false;
    s_cred_pass_null = false;
    memset(s_sub_ptrs, 0, sizeof(s_sub_ptrs));
    rb_flush(&s_ah_mqtt_rb, nullptr);
}

/** @brief 保证装配跑过一次（队列/主题/订阅都已就绪） */
static void _ensure_init(void)
{
    if (!s_inited) {
        _ah_mqtt_module_init();
        s_inited = true;
    }
}

/** @brief 向协议环形缓冲喂一条消息（可带结尾 NUL） */
static void _feed(const char *payload, uint16_t len)
{
    rb_flush(&s_ah_mqtt_rb, nullptr);
    rb_write(&s_ah_mqtt_rb, (const uint8_t *)payload, len, nullptr);
}

/* ================================================================
 *  装配
 * ================================================================ */

/** @brief 配置缺设备类型：主题构建失败，不得注入 broker/订阅 */
static void case_init_bad_cfg(void)
{
    TEST_BEGIN("装配：设备类型为 0 → 不启动，不碰通道");
    _reset();

    s_cfg.device_id = 0U;
    _ah_mqtt_module_init();

    CHECK_MSG(s_broker_calls == 0, "配置非法不应设置 broker");
    CHECK_MSG(s_cred_calls == 0, "配置非法不应设置凭据");
    CHECK_MSG(s_sub_calls == 0, "配置非法不应登记订阅");
    CHECK_MSG(g_ah_mqtt_msg_queue == nullptr, "配置非法不应建队列");
}

/** @brief 订阅表登记失败：不建队列/任务（宁可不起，也不静默没订上） */
static void case_init_subscribe_fail(void)
{
    TEST_BEGIN("装配：订阅登记失败 → 不建队列/任务");
    _reset();

    s_sub_rc = -1;
    _ah_mqtt_module_init();

    CHECK_MSG(s_broker_calls == 1, "配置合法应先注入 broker");
    CHECK_MSG(s_sub_calls == 1, "应尝试登记一次订阅");
    CHECK_MSG(s_task_calls == 0, "登记失败不应建任务");
    CHECK_MSG(g_ah_mqtt_msg_queue == nullptr, "登记失败不应建队列");
}

/** @brief 正常装配：主题/ClientID/Broker/凭据/订阅全链路 */
static void case_init_ok(void)
{
    TEST_BEGIN("装配：主题前缀 + 4 条订阅 + 上报主题 + ClientID/凭据");
    _reset();

    _ah_mqtt_module_init();

    const app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;
    CHECK_MSG(strcmp(p->prefix, "52011314/01/32/01") == 0, "主题前缀不对：%s", p->prefix);
    CHECK_MSG(strcmp(p->sub_topics[0], "52011314/01/32/01/ASK/board/NULL") == 0, "显示订阅主题不对");
    CHECK_MSG(strcmp(p->sub_topics[2], "52011314/01/32/01/ASK/op/restart") == 0, "重启订阅主题不对");
    CHECK_MSG(strcmp(p->push_status_topic, "52011314/01/32/01/Push/monitor/devstatus") == 0,
              "状态上报主题不对");
    CHECK_MSG(strcmp(p->push_sign_topic, "52011314/01/32/01/Push/monitor/sign") == 0,
              "签到主题不对");

    CHECK_MSG(s_broker_calls == 1 && s_broker_port == 6000U, "Broker 端口应为 6000");
    CHECK_MSG(memcmp(s_broker_ip, s_cfg.mqtt_ip, 4) == 0, "Broker IP 应来自配置");
    CHECK_MSG(strcmp(s_cred_id, "CD52011314013201") == 0, "Client ID 不对：%s", s_cred_id);
    CHECK_MSG(!s_cred_user_null && strcmp(s_cred_user, "pxh") == 0, "用户名应为 pxh");
    CHECK_MSG(s_cred_pass_null, "空密码必须注入 nullptr（lwIP 对空串返回 ERR_VAL）");

    CHECK_MSG(s_sub_calls == 1 && s_sub_count == 4U, "应登记 4 条订阅");
    CHECK_MSG(s_sub_ptrs[1] == p->sub_topics[1], "订阅表应指向协议持有的完整主题");
    CHECK_MSG(s_bind_calls == 1, "应绑定到 MQTT 通道");
    CHECK_MSG(s_task_calls == 1, "应创建协议任务");
    CHECK_MSG(g_ah_mqtt_msg_queue != nullptr, "应建好帧队列");
    s_inited = true;
}

/** @brief 定长凭据转字符串：无 NUL 的 10 字节也要完整保留 */
static void case_ah_mqtt_cred_to_str(void)
{
    TEST_BEGIN("凭据：定长 10 字节（可无 NUL）转 NUL 结尾");
    _reset();

    char out[APP_AH_MQTT_CRED_LEN + 1U];
    _ah_mqtt_cred_to_str("pxh", sizeof("pxh"), out, sizeof(out));
    CHECK_MSG(strcmp(out, "pxh") == 0, "短串应原样输出");

    _ah_mqtt_cred_to_str("0123456789", APP_AH_MQTT_CRED_LEN, out, sizeof(out));
    CHECK_MSG(strcmp(out, "0123456789") == 0, "无 NUL 的 10 字节应完整保留");

    _ah_mqtt_cred_to_str("", 1U, out, sizeof(out));
    CHECK_MSG(out[0] == '\0', "空串应输出空串");
}

/** @brief 上报通知号：RTC 时间 + 配置身份 + 自增序号 */
static void case_report_notify(void)
{
    TEST_BEGIN("上报通知号：RTC 时间 + 身份 + 自增");
    _reset();

    app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;
    app_ah_notify_build(&p->notify_in, 0U, &p->cfg, 0U); /* 与上报号解耦，避免误用 */
    p->cfg       = s_cfg;
    p->send_seq  = 7U;
    s_rtc_ts     = 1741183200U;

    app_ah_notify_id_t id;
    _ah_mqtt_notify_for_report(p, &id);
    CHECK_MSG(memcmp(&id, "20250305143201000007", APP_AH_NOTIFY_ID_LEN) == 0, "通知号不对");

    _ah_mqtt_notify_for_report(p, &id);
    CHECK_MSG(memcmp(&id, "20250305143201000008", APP_AH_NOTIFY_ID_LEN) == 0, "序号应自增");
    CHECK_MSG(p->send_seq == 9U, "发送计数应递增到 9");
}

/* ================================================================
 *  探针
 * ================================================================ */

/** @brief 完整主题 → 命令号；未订阅主题 → SKIP */
static void case_probe(void)
{
    TEST_BEGIN("探针：完整主题分类 / 未订阅主题 SKIP");
    _reset();
    _ensure_init();

    const app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;
    uint8_t                    scratch[32];
    uint32_t                   total = 0U;
    uint8_t                    aux   = 0xFFU;

    _feed("ABCD\0", 5U);
    const app_ccb_src_t src = {.topic = p->sub_topics[1]};
    app_pcb_probe_state_t st =
        app_ah_mqtt_probe_frame(&s_ah_mqtt_proto.base, nullptr, &src, scratch, sizeof(scratch),
                                &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_READY, "已订阅主题应 READY，实际 %d", (int)st);
    CHECK_MSG(aux == 1U, "命令号应为 1，实际 %u", (unsigned)aux);
    CHECK_MSG(total == 5U, "整帧长度含 NUL 应为 5，实际 %u", (unsigned)total);

    _feed("ABCD\0", 5U);
    const app_ccb_src_t unknown = {.topic = "52011314/01/32/01/ASK/unknown"};
    st = app_ah_mqtt_probe_frame(&s_ah_mqtt_proto.base, nullptr, &unknown, scratch, sizeof(scratch),
                                 &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_SKIP, "未订阅主题应 SKIP，实际 %d", (int)st);
}

/** @brief 结尾 NUL 未到 → WAIT；暂存区满仍无 NUL → SKIP */
static void case_probe_nul(void)
{
    TEST_BEGIN("探针：无 NUL → WAIT，暂存区满 → SKIP");
    _reset();
    _ensure_init();

    uint8_t  scratch[16];
    uint32_t total = 0U;
    uint8_t  aux   = 0U;
    _feed("ABCD", 4U);

    app_pcb_probe_state_t st =
        app_ah_mqtt_probe_frame(&s_ah_mqtt_proto.base, nullptr, nullptr, scratch, sizeof(scratch),
                                &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_WAIT, "无 NUL 且未满应 WAIT，实际 %d", (int)st);

    _feed("0123456789ABCDEF", 16U);
    st = app_ah_mqtt_probe_frame(&s_ah_mqtt_proto.base, nullptr, nullptr, scratch, sizeof(scratch),
                                 &total, &aux);
    CHECK_MSG(st == APP_PCB_PROBE_STATE_SKIP, "暂存区满仍无 NUL 应 SKIP，实际 %d", (int)st);
    CHECK_MSG(total == 16U, "SKIP 应丢弃整段 16 字节");
}

/* ================================================================ */

int main(void)
{
    case_init_bad_cfg();
    case_init_subscribe_fail();
    case_init_ok();
    case_ah_mqtt_cred_to_str();
    case_report_notify();
    case_probe();
    case_probe_nul();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
