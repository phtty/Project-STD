/**
 * @file    app_ah_mqtt.c
 * @brief   AH 平台 MQTT 应用层协议（业务信道）
 *
 * 上电（`sw_post`）从 `app_ah_cfg` 取一份快照，派生整条链路需要的身份：
 *   - 主题前缀 `站8/道2/型2/号2`（LaneHex + device_id/device_no）；
 *   - 4 条订阅主题、2 条上报主题、回复主题（前缀 + 命令表后缀）；
 *   - Client ID、Broker 地址、MQTT 用户名/密码（注入 app_mqtt 通道）。
 *
 * **主题只在启动时构建一次**：0AH 改参数后统一重启生效，运行期不重建主题，
 * 因此上报任务、探针和命令处理可以无锁共享这份快照。
 *
 * 订阅命令表列出的 ASK 主题，按来源主题分派命令；另有两个周期上报任务
 * （状态 10s / 签到 5min）。承载通道：MQTT。
 */

#include "app_ah_mqtt.h"

#include "FreeRTOS.h"
#include "app_ah_cfg.h"
#include "app_ah_mqtt_cmd.h"
#include "app_mqtt.h"
#include "dev_display.h"
#include "initcall.h"
#include "pl_mem.h"
#include "pl_rtc.h"
#include "pl_task.h"

#include <stdio.h>
#include <string.h>

#define AH_MQTT_PAYLOAD_MAX (533U) /* MQTT_FRAME_MAX_LEN */

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ---- */
/* RB 容量须取「2 × 最长帧」与「传输层单次最大写入」的较大者：MQTT 通道单次派发
   最多为 rcv_buf 的 1044 字节，故 2048 同时覆盖两者。 */
RB_DEFINE_ATTR(s_ah_mqtt_rb, 2048, PL_CCMRAM);

static const app_pcb_ops_t s_ah_mqtt_ops = {.probe = app_ah_mqtt_probe_frame};

/* 派生协议对象：base 第一个成员（container_of 偏移 0），身份/主题/通知号收在这里，
   不再散落成文件级全局。 */
static app_ah_mqtt_proto_t s_ah_mqtt_proto = {
    .base = {
        .name        = "ah_mqtt",
        .ops         = &s_ah_mqtt_ops,
        .rb          = &s_ah_mqtt_rb,
        .payload_max = AH_MQTT_PAYLOAD_MAX,
    },
};

static_assert(AH_MQTT_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "AH/MQTT 最长帧超过框架暂存上限");

osMessageQueueId_t g_ah_mqtt_msg_queue;

/** 订阅表：由完整主题数组派生（指针数组），通道长期持有其指针，故须为静态存储期 */
static const char *s_sub_topics[AH_MQTT_CMD_COUNT];

/* ---- 帧队列静态分配：在 initcall 内建好 ----
   通道任务可能早于协议任务首次运行就投递帧，在协议任务里建队列会留下
   "向空队列投递"的窗口。 */
#define AH_MQTT_MSG_SIZE (sizeof(app_dispatch_msg_t) + AH_MQTT_PAYLOAD_MAX)

static StaticQueue_t s_ah_mqtt_queue_cb;
static uint8_t       s_ah_mqtt_queue_buf[AH_MQTT_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_ah_mqtt_queue_attr = {
    .name    = "g_ah_mqtt_msg_queue",
    .cb_mem  = &s_ah_mqtt_queue_cb,
    .cb_size = sizeof(s_ah_mqtt_queue_cb),
    .mq_mem  = s_ah_mqtt_queue_buf,
    .mq_size = sizeof(s_ah_mqtt_queue_buf),
};

osThreadId_t g_ah_mqtt_task_handle;
const osThreadAttr_t g_ah_mqtt_task_attr = {
    .name       = "app_ah_mqtt_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

static osThreadId_t s_sign_up_task_handle;
static const osThreadAttr_t s_sign_up_task_attr = {
    .name       = "_ah_mqtt_sign_up_task",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

static osThreadId_t s_report_task_handle;
static const osThreadAttr_t s_report_task_attr = {
    .name       = "_ah_mqtt_report_task",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

static void _ah_mqtt_sign_up_task(void *argument);
static void _ah_mqtt_report_task(void *argument);

/** @brief 在完整订阅主题里查找来源主题；命中则给出命令号（即表索引） */
static bool _ah_mqtt_topic_index(const app_ah_mqtt_proto_t *p, const char *topic, uint8_t *cmd)
{
    if (p == nullptr || topic == nullptr)
        return false;

    for (uint8_t i = 0; i < AH_MQTT_CMD_COUNT; i++) {
        if (strcmp(p->sub_topics[i], topic) == 0) {
            *cmd = i;
            return true;
        }
    }
    return false;
}

void app_ah_mqtt_task(void *argument)
{
    (void)argument;

    static uint8_t      msg_buf[AH_MQTT_MSG_SIZE];
    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)msg_buf;

    while (g_mqtt_ccb.state != APP_MQTT_STATE_READY) {
        osDelay(100);
    }
    s_sign_up_task_handle = pl_task_new(_ah_mqtt_sign_up_task, nullptr, &s_sign_up_task_attr);
    s_report_task_handle  = pl_task_new(_ah_mqtt_report_task, nullptr, &s_report_task_attr);

    for (;;) {
        if (osOK != osMessageQueueGet(g_ah_mqtt_msg_queue, msg, nullptr, osWaitForever))
            continue;

        /* 命令分类由探针在投递当下完成、经 msg->aux 传来。这里不再回头解析通道上的
           topic —— 那是"per-message 的值寄存在长生命周期对象上"，突发两条消息时
           会被后一条覆盖。仍按表长做范围校验：aux 是探针的输出，怀疑它等于怀疑探针。 */
        uint8_t cmd = msg->aux;
        if (cmd < AH_MQTT_CMD_COUNT)
            g_ah_mqtt_cmd_table[cmd].handler(&s_ah_mqtt_proto.base, msg->ccb, (char *)(msg->data));
    }
}

app_pcb_probe_state_t app_ah_mqtt_probe_frame(app_pcb_t *self, const app_ccb_t *ccb,
                                              const app_ccb_src_t *src, uint8_t *scratch,
                                              uint16_t scratch_size, uint32_t *total_len,
                                              uint8_t *aux)
{
    (void)ccb;

    /* 一条 MQTT 消息即一帧，以结尾 NUL 定界（通道投递前补的），据此逐条成帧 ——
       连续两条消息同处缓冲区时也能各自成帧，不必依赖通道给出长度（那是 per-message
       的值，寄存在通道对象上会被后一条消息覆盖）。 */
    uint16_t n = rb_peek(self->rb, 0, scratch, scratch_size, nullptr);
    if (n == 0)
        return APP_PCB_PROBE_STATE_WAIT;

    const uint8_t *z = memchr(scratch, 0, n);
    if (z == nullptr) {
        /* 整段暂存区里都没有结尾 NUL：帧长超过暂存区，无法定界。此时不能一直 WAIT
           （缓冲区只进不出），整段丢弃更安全。 */
        if (n >= scratch_size) {
            *total_len = n;
            return APP_PCB_PROBE_STATE_SKIP;
        }
        return APP_PCB_PROBE_STATE_WAIT; /* 结尾 NUL 未到齐 */
    }

    *total_len = (uint32_t)(z - scratch) + 1; /* 含结尾 NUL，命令处理按字符串使用 */

    /* 分类：来源主题 → 本协议命令号。不是本协议订阅的主题（同通道上可能有别的协议
       订阅了别的主题）→ 整帧跳过。 */
    uint8_t               cmd = 0;
    app_ah_mqtt_proto_t *p   = container_of(self, app_ah_mqtt_proto_t, base);
    if (!_ah_mqtt_topic_index(p, src != nullptr ? src->topic : nullptr, &cmd))
        return APP_PCB_PROBE_STATE_SKIP;

    *aux = cmd;
    return APP_PCB_PROBE_STATE_READY;
}

/** @brief 生成上报通知号；失败（时间未设/配置异常）时填 0，避免发出半截号 */
static void _ah_mqtt_notify_for_report(app_ah_mqtt_proto_t *p, app_ah_notify_id_t *out)
{
    uint32_t ts = pl_rtc_get_timestamp(pl_rtc_get_handle());
    if (!app_ah_notify_build(out, ts, &p->cfg, p->send_seq++))
        memset(out, 0, sizeof(*out));
}

static void _ah_mqtt_report_task(void *argument)
{
    (void)argument;

    app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;

    static app_ah_mqtt_state_report_t report = {
        .work_status = '0',
        .reserved    = "00000000",
    };

    for (;;) {
        _ah_mqtt_notify_for_report(p, &report.notify);
        report.run_status = dev_display_get()->light_level ? '1' : '0';
        /* 长度显式给出：保持原有 strlen 语义（结构体内含 NUL 时按字符串截断） */
        app_mqtt_send(p->push_status_topic, &report, (uint16_t)strlen((char *)&report));
        osDelay(10 * 1000); // 10秒上报1次
    }
}

static void _ah_mqtt_sign_up_task(void *argument)
{
    (void)argument;

    app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;

    static app_ah_mqtt_sign_up_t sign_up = {
        .type         = '1',
        .work_status  = '0',
        .soft_ver     = "SoftV1.0.0",
        .hard_ver     = "FirmwareV1",
        .protocol_ver = "LEDV1.0.00",
        .company      = "CQChuangDi",
        .device       = "LEDScreen1",
        .reserved     = "00000000000000000000",
    };

    _ah_mqtt_notify_for_report(p, &sign_up.notify);
    app_mqtt_send(p->push_sign_topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    sign_up.type = '0';

    for (;;) {
        osDelay(5 * 60 * 1000); // 5分钟签到1次
        _ah_mqtt_notify_for_report(p, &sign_up.notify);
        app_mqtt_send(p->push_sign_topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    }
}

/** @brief 把定长 10 字节凭据转成 NUL 结尾字符串（不足补 NUL；可能没有终止符） */
static void _ah_mqtt_cred_to_str(const char *src, size_t src_len, char *out, size_t cap)
{
    size_t len = 0U;
    while (len < src_len && src[len] != '\0')
        len++;
    if (len >= cap)
        len = cap - 1U;
    memcpy(out, src, len);
    out[len] = '\0';
}

/** @brief 用配置快照拼出全部主题；任一截断即失败（宁可不启动，也不订错主题） */
static bool _ah_mqtt_build_topics(app_ah_mqtt_proto_t *p)
{
    if (!app_ah_topic_prefix_build(&p->cfg, p->prefix, sizeof(p->prefix)))
        return false;

    for (uint8_t i = 0; i < AH_MQTT_CMD_COUNT; i++) {
        int n = snprintf(p->sub_topics[i], sizeof(p->sub_topics[i]), "%s/%s", p->prefix,
                         g_ah_mqtt_cmd_table[i].suffix);
        if (n <= 0 || (size_t)n >= sizeof(p->sub_topics[i]))
            return false;
    }

    int n = snprintf(p->push_status_topic, sizeof(p->push_status_topic),
                     "%s/Push/monitor/devstatus", p->prefix);
    if (n <= 0 || (size_t)n >= sizeof(p->push_status_topic))
        return false;

    n = snprintf(p->push_sign_topic, sizeof(p->push_sign_topic), "%s/Push/monitor/sign", p->prefix);
    return n > 0 && (size_t)n < sizeof(p->push_sign_topic);
}

/* ---- 自注册到 app_dispatch ---- */
static void _ah_mqtt_module_init(void)
{
    app_ah_mqtt_proto_t *p = &s_ah_mqtt_proto;

    app_ah_cfg_get(&p->cfg);
    if (!_ah_mqtt_build_topics(p)) {
        printf("[ah/mqtt] 主题构建失败（LaneHex/设备类型非法），MQTT 业务不启动\n");
        return;
    }

    char client_id[APP_AH_CLIENT_ID_MAX];
    if (!app_ah_client_id_build(&p->cfg, client_id, sizeof(client_id))) {
        printf("[ah/mqtt] Client ID 构建失败，MQTT 业务不启动\n");
        return;
    }

    /* 定长凭据可能没有 NUL；空串必须传 nullptr —— lwIP 对“非 NULL 但长度 0”的
       用户名/密码直接返回 ERR_VAL，等于永远连不上。 */
    char user[APP_AH_MQTT_CRED_LEN + 1U];
    char pwd[APP_AH_MQTT_CRED_LEN + 1U];
    _ah_mqtt_cred_to_str(p->cfg.mqtt_user, sizeof(p->cfg.mqtt_user), user, sizeof(user));
    _ah_mqtt_cred_to_str(p->cfg.mqtt_pwd, sizeof(p->cfg.mqtt_pwd), pwd, sizeof(pwd));

    app_mqtt_set_broker(p->cfg.mqtt_ip, p->cfg.mqtt_port);
    app_mqtt_set_credentials(client_id, user[0] != '\0' ? user : nullptr,
                             pwd[0] != '\0' ? pwd : nullptr);

    /* 订阅哪些主题是协议知识：登记一次，通道在每次连接（含重连）就绪时施加 */
    for (uint8_t i = 0; i < AH_MQTT_CMD_COUNT; i++)
        s_sub_topics[i] = p->sub_topics[i];
    if (app_mqtt_subscribe(s_sub_topics, AH_MQTT_CMD_COUNT) < 0) {
        printf("[ah/mqtt] 订阅表登记失败（通道表满），MQTT 业务不启动\n");
        return;
    }

    rb_init(&s_ah_mqtt_rb, "ah_mqtt");

    /* 队列在此建好：通道任务可能早于协议任务首次运行就投递帧 */
    g_ah_mqtt_msg_queue        = osMessageQueueNew(1, AH_MQTT_MSG_SIZE, &s_ah_mqtt_queue_attr);
    s_ah_mqtt_proto.base.queue = g_ah_mqtt_msg_queue;
    if (g_ah_mqtt_msg_queue == nullptr) {
        printf("[ah/mqtt] 帧队列创建失败，MQTT 业务不启动\n");
        return;
    }

    app_dispatch_bind(&s_ah_mqtt_proto.base, app_mqtt_ccb());

    // 创建协议处理任务
    g_ah_mqtt_task_handle = pl_task_new(app_ah_mqtt_task, nullptr, &g_ah_mqtt_task_attr);
    if (g_ah_mqtt_task_handle == nullptr) {
        printf("[ah/mqtt] 协议任务创建失败，MQTT 业务不启动\n");
        return;
    }

    printf("[ah/mqtt] 主题前缀 %s，Broker %u.%u.%u.%u:%u\n", p->prefix,
           (unsigned)p->cfg.mqtt_ip[0], (unsigned)p->cfg.mqtt_ip[1], (unsigned)p->cfg.mqtt_ip[2],
           (unsigned)p->cfg.mqtt_ip[3], (unsigned)p->cfg.mqtt_port);
}
sw_post_initcall(_ah_mqtt_module_init);
