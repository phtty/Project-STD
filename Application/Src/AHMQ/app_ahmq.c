/**
 * @file    app_ahmq.c
 * @brief   AH 平台 MQTT 应用层协议（第二代分发引擎）
 *
 * 订阅命令表列出的 ASK 主题，按来源主题分派命令；另有两个周期上报任务
 * （状态 10s / 签到 5min）。承载通道：MQTT。
 *
 * 当前状态：模块 initcall 处于注释状态，链路未激活（详见文件末尾）。
 */

#include "app_ahmq.h"

#include "FreeRTOS.h"
#include "app_ahmq_cmd.h"
#include "dev_display.h"
#include "app_mqtt.h"
#include "initcall.h"

#include <string.h>
#include "pl_task.h"

#define AHMQ_PAYLOAD_MAX (533U) /* MQTT_FRAME_MAX_LEN */

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ---- */
/* RB 容量须取「2 × 最长帧」与「传输层单次最大写入」的较大者：MQTT 通道单次派发
   最多为 rcv_buf 的 1044 字节，故 2048 同时覆盖两者。 */
RB_DEFINE(s_ahmq_rb, 2048);

static const app_pcb_ops_t s_ahmq_ops = {.probe = app_ahmq_probe_frame};

/* 派生协议对象：base 第一个成员（container_of 偏移 0），协议自有状态（设备标识、
 * 通知号、回复主题）收在这里，不再散落成文件级全局。 */
static app_ahmq_proto_t s_ahmq = {
    .base =
        {
            .name        = "ahmq",
            .ops         = &s_ahmq_ops,
            .rb          = &s_ahmq_rb,
            .payload_max = AHMQ_PAYLOAD_MAX,
        },
    .topic_info =
        {
            .station_hex = "11451419",
            .lane_hex    = "01",
            .device_type = "32",
            .device_id   = "01",
        },
    .notify_id =
        {
            .date_time =
                {
                    .year  = "2025",
                    .month = "03",
                    .day   = "05",
                    .hour  = "14",
                },
            .device_type = "32",
            .device_id   = "01",
            .send_count  = "000000",
        },
};

static_assert(AHMQ_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "AHMQ 最长帧超过框架暂存上限");

osMessageQueueId_t g_ahmq_msg_queue;

/** 订阅表：由命令表派生（主题指针数组），通道长期持有其指针，故须为静态存储期 */
static const char *s_sub_topics[AHMQ_CMD_COUNT];

/* ---- 帧队列静态分配：在 initcall 内建好 ----
   通道任务可能早于协议任务首次运行就投递帧，在协议任务里建队列会留下
   "向空队列投递"的窗口。 */
#define AHMQ_MSG_SIZE (sizeof(app_dispatch_msg_t) + AHMQ_PAYLOAD_MAX)

static StaticQueue_t s_ahmq_queue_cb;
static uint8_t s_ahmq_queue_buf[AHMQ_MSG_SIZE];
static const osMessageQueueAttr_t s_ahmq_queue_attr = {
    .name    = "g_ahmq_msg_queue",
    .cb_mem  = &s_ahmq_queue_cb,
    .cb_size = sizeof(s_ahmq_queue_cb),
    .mq_mem  = s_ahmq_queue_buf,
    .mq_size = sizeof(s_ahmq_queue_buf),
};

osThreadId_t g_ahmq_task_handle;
const osThreadAttr_t g_ahmq_task_attr = {
    .name       = "app_ahmq_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

static osThreadId_t s_sign_up_task_handle;
static const osThreadAttr_t s_sign_up_task_attr = {
    .name       = "_SignUpTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

static osThreadId_t s_report_task_handle;
static const osThreadAttr_t s_report_task_attr = {
    .name       = "_ReportTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

static void _SignUpTask(void *argument);
static void _ReportTask(void *argument);

/** @brief 在命令表中查找来源主题；命中则给出命令号（即表索引）*/
static bool _ahmq_topic_index(const char *topic, uint8_t *cmd)
{
    for (uint8_t i = 0; i < AHMQ_CMD_COUNT; i++) {
        if (strcmp(g_ahmq_cmd_table[i].topic, topic) == 0) {
            *cmd = i;
            return true;
        }
    }
    return false;
}

void app_ahmq_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[AHMQ_MSG_SIZE];
    app_dispatch_msg_t *msg = (app_dispatch_msg_t *)_msg_buf;

    while (g_mqtt_ccb.state != APP_MQTT_STATE_READY) {
        osDelay(100);
    }
    s_sign_up_task_handle = pl_task_new(_SignUpTask, NULL, &s_sign_up_task_attr);
    s_report_task_handle = pl_task_new(_ReportTask, NULL, &s_report_task_attr);

    for (;;) {
        if (osOK != osMessageQueueGet(g_ahmq_msg_queue, msg, NULL, osWaitForever)) {
            continue;
        }

        /* 命令分类由探针在投递当下完成、经 msg->aux 传来。这里不再回头解析通道上的
           topic —— 那是"per-message 的值寄存在长生命周期对象上"，突发两条消息时
           会被后一条覆盖。仍按表长做范围校验：aux 是探针的输出，怀疑它等于怀疑探针。 */
        uint8_t cmd = msg->aux;
        if (cmd < AHMQ_CMD_COUNT)
            g_ahmq_cmd_table[cmd].handler(&s_ahmq.base, msg->ccb, (char *)(msg->data));
    }
}

app_pcb_probe_state_t app_ahmq_probe_frame(app_pcb_t *self, const app_ccb_t *ccb, const app_ccb_src_t *src,
                                    uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                    uint8_t *aux)
{
    (void)ccb;

    /* 一条 MQTT 消息即一帧，以结尾 NUL 定界（通道投递前补的），据此逐条成帧 ——
       连续两条消息同处缓冲区时也能各自成帧，不必依赖通道给出长度（那是 per-message
       的值，寄存在通道对象上会被后一条消息覆盖）。 */
    uint16_t n = rb_peek_capped(self->rb, 0, scratch, scratch_size, nullptr);
    if (n == 0) return APP_PCB_PROBE_STATE_WAIT;

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
    uint8_t cmd = 0;
    if (src == nullptr || src->topic == nullptr || !_ahmq_topic_index(src->topic, &cmd))
        return APP_PCB_PROBE_STATE_SKIP;

    *aux = cmd;
    return APP_PCB_PROBE_STATE_READY;
}

static void _ReportTask(void *argument)
{
    (void)argument;

    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/devstatus",
             s_ahmq.topic_info.station_hex,
             s_ahmq.topic_info.lane_hex,
             s_ahmq.topic_info.device_type,
             s_ahmq.topic_info.device_id);

    static app_ahmq_state_report_t report = {
        .work_status = '0',
        .reserved = "00000000",
    };

    for (;;) {
        memcpy(&(report.notify), &s_ahmq.notify_id, sizeof(report.notify));
        report.run_status = dev_display_get()->light_level ? '1' : '0'; // 根据当前亮度判断是否开显示屏
        /* 长度显式给出：保持原有 strlen 语义（结构体内含 NUL 时按字符串截断） */
        app_mqtt_send(topic, &report, (uint16_t)strlen((char *)&report));
        osDelay(10 * 1000); // 10秒上报1次
    }
}

static void _SignUpTask(void *argument)
{
    (void)argument;

    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/sign",
             s_ahmq.topic_info.station_hex,
             s_ahmq.topic_info.lane_hex,
             s_ahmq.topic_info.device_type,
             s_ahmq.topic_info.device_id);

    static app_ahmq_sign_up_t sign_up = {
        .type         = '1',
        .work_status     = '0',
        .soft_ver     = "SoftV1.0.0",
        .hard_ver     = "FirmwareV1",
        .protocol_ver = "LEDV1.0.00",
        .company      = "CQChuangDi",
        .device       = "LEDScreen1",
        .reserved     = "00000000000000000000",
    };
    memcpy(&(sign_up.notify), &s_ahmq.notify_id, sizeof(sign_up.notify));
    app_mqtt_send(topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    sign_up.type = '0';

    for (;;) {
        osDelay(5 * 60 * 1000); // 5分钟签到1次
        memcpy(&(sign_up.notify), &s_ahmq.notify_id, sizeof(sign_up.notify));
        app_mqtt_send(topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    }
}

/* ---- 自注册到 app_dispatch ---- */
[[maybe_unused]] static void _ahmq_module_init(void)
{
    rb_init(&s_ahmq_rb, "ahmq");

    /* 队列在此建好：通道任务可能早于协议任务首次运行就投递帧 */
    g_ahmq_msg_queue = osMessageQueueNew(1, AHMQ_MSG_SIZE, &s_ahmq_queue_attr);
    s_ahmq.base.queue  = g_ahmq_msg_queue;

    /* 订阅哪些主题是协议知识：登记一次，通道在每次连接（含重连）就绪时施加 */
    for (uint8_t i = 0; i < AHMQ_CMD_COUNT; i++)
        s_sub_topics[i] = g_ahmq_cmd_table[i].topic;
    app_mqtt_subscribe(s_sub_topics, AHMQ_CMD_COUNT);

    app_dispatch_bind(&s_ahmq.base, app_mqtt_ccb());

    // 创建协议处理任务
    g_ahmq_task_handle = pl_task_new(app_ahmq_task, NULL, &g_ahmq_task_attr);
}
/* 保持注释：本模块当前不启用（app_mqtt_start 亦无调用者，MQTT 链路未激活）。 */
// sw_app_initcall(_ahmq_module_init);
