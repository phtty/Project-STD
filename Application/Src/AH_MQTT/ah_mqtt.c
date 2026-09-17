/**
 * @file    ah_mqtt.c
 * @brief   AH 平台 MQTT 应用层协议（第二代分发引擎）
 *
 * 订阅命令表列出的 ASK 主题，按来源主题分派命令；另有两个周期上报任务
 * （状态 10s / 签到 5min）。承载通道：MQTT。
 *
 * 当前状态：模块 initcall 处于注释状态，链路未激活（详见文件末尾）。
 */

#include "ah_mqtt.h"

#include "FreeRTOS.h"
#include "ah_mqtt_cmd.h"
#include "dev_display.h"
#include "app_mqtt.h"
#include "initcall.h"

#include <string.h>
#include "pl_task.h"

#define AH_MQTT_PAYLOAD_MAX (533U) /* MQTT_FRAME_MAX_LEN */

/* ---- 协议控制块：协议自有缓冲区与队列，静态持有 ---- */
/* RB 容量须取「2 × 最长帧」与「传输层单次最大写入」的较大者：MQTT 通道单次派发
   最多为 rcv_buf 的 1044 字节，故 2048 同时覆盖两者。 */
RB_DEFINE(s_ah_mqtt_rb, 2048);

static const pcb_ops_t s_ah_mqtt_ops = {.probe = ah_mqtt_probe_frame};

/* 派生协议对象：base 第一个成员（container_of 偏移 0），协议自有状态（设备标识、
 * 通知号、回复主题）收在这里，不再散落成文件级全局。 */
static ah_mqtt_proto_t s_ah_mqtt = {
    .base =
        {
            .name        = "ah_mqtt",
            .ops         = &s_ah_mqtt_ops,
            .rb          = &s_ah_mqtt_rb,
            .payload_max = AH_MQTT_PAYLOAD_MAX,
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

static_assert(AH_MQTT_PAYLOAD_MAX <= FRAME_DATA_MAX_LEN, "AH_MQTT 最长帧超过框架暂存上限");

osMessageQueueId_t g_proto_ah_matt_queue;

/** 订阅表：由命令表派生（主题指针数组），通道长期持有其指针，故须为静态存储期 */
static const char *s_sub_topics[AH_MQTT_CMD_COUNT];

/* ---- 帧队列静态分配：在 initcall 内建好 ----
   通道任务可能早于协议任务首次运行就投递帧，在协议任务里建队列会留下
   "向空队列投递"的窗口。 */
#define AH_MQTT_MSG_SIZE (sizeof(frame_msg_t) + AH_MQTT_PAYLOAD_MAX)

static StaticQueue_t s_ah_mqtt_queue_cb;
static uint8_t s_ah_mqtt_queue_buf[AH_MQTT_MSG_SIZE];
static const osMessageQueueAttr_t s_ah_mqtt_queue_attr = {
    .name    = "g_proto_ah_matt_queue",
    .cb_mem  = &s_ah_mqtt_queue_cb,
    .cb_size = sizeof(s_ah_mqtt_queue_cb),
    .mq_mem  = s_ah_mqtt_queue_buf,
    .mq_size = sizeof(s_ah_mqtt_queue_buf),
};

osThreadId_t g_ah_mqtt_task_handle;
const osThreadAttr_t ProtocolTask_attributes = {
    .name       = "ah_mqtt_handle_task",
    .stack_size = 512 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

osThreadId_t SignUpHandle;
const osThreadAttr_t SignUpTask_attributes = {
    .name       = "SignUpTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

osThreadId_t ReportHandle;
const osThreadAttr_t ReportTask_attributes = {
    .name       = "ReportTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

static void SignUpTask(void *argument);
static void ReportTask(void *argument);

/** @brief 在命令表中查找来源主题；命中则给出命令号（即表索引）*/
static bool ah_mqtt_topic_index(const char *topic, uint8_t *cmd)
{
    for (uint8_t i = 0; i < AH_MQTT_CMD_COUNT; i++) {
        if (strcmp(g_ah_mqtt_cmd_table[i].topic, topic) == 0) {
            *cmd = i;
            return true;
        }
    }
    return false;
}

void ah_mqtt_handle_task(void *argument)
{
    (void)argument;

    static uint8_t _msg_buf[AH_MQTT_MSG_SIZE];
    frame_msg_t *msg = (frame_msg_t *)_msg_buf;

    while (g_mqtt.state != MQTT_ST_READY) {
        osDelay(100);
    }
    SignUpHandle = pl_task_new(SignUpTask, NULL, &SignUpTask_attributes);
    ReportHandle = pl_task_new(ReportTask, NULL, &ReportTask_attributes);

    for (;;) {
        if (osOK != osMessageQueueGet(g_proto_ah_matt_queue, msg, NULL, osWaitForever)) {
            continue;
        }

        /* 命令分类由探针在投递当下完成、经 msg->aux 传来。这里不再回头解析通道上的
           topic —— 那是"per-message 的值寄存在长生命周期对象上"，突发两条消息时
           会被后一条覆盖。仍按表长做范围校验：aux 是探针的输出，怀疑它等于怀疑探针。 */
        uint8_t cmd = msg->aux;
        if (cmd < AH_MQTT_CMD_COUNT)
            g_ah_mqtt_cmd_table[cmd].handler(&s_ah_mqtt.base, msg->ccb, (char *)(msg->data));
    }
}

pcb_probe_sta_t ah_mqtt_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                    uint8_t *scratch, uint16_t scratch_size, uint32_t *total_len,
                                    uint8_t *aux)
{
    (void)ccb;

    /* 一条 MQTT 消息即一帧，以结尾 NUL 定界（通道投递前补的），据此逐条成帧 ——
       连续两条消息同处缓冲区时也能各自成帧，不必依赖通道给出长度（那是 per-message
       的值，寄存在通道对象上会被后一条消息覆盖）。 */
    uint16_t n = rb_peek_capped(self->rb, 0, scratch, scratch_size, nullptr);
    if (n == 0) return PCB_PROBE_WAIT;

    const uint8_t *z = memchr(scratch, 0, n);
    if (z == nullptr) {
        /* 整段暂存区里都没有结尾 NUL：帧长超过暂存区，无法定界。此时不能一直 WAIT
           （缓冲区只进不出），整段丢弃更安全。 */
        if (n >= scratch_size) {
            *total_len = n;
            return PCB_PROBE_SKIP;
        }
        return PCB_PROBE_WAIT; /* 结尾 NUL 未到齐 */
    }

    *total_len = (uint32_t)(z - scratch) + 1; /* 含结尾 NUL，命令处理按字符串使用 */

    /* 分类：来源主题 → 本协议命令号。不是本协议订阅的主题（同通道上可能有别的协议
       订阅了别的主题）→ 整帧跳过。 */
    uint8_t cmd = 0;
    if (src == nullptr || src->topic == nullptr || !ah_mqtt_topic_index(src->topic, &cmd))
        return PCB_PROBE_SKIP;

    *aux = cmd;
    return PCB_PROBE_READY;
}

void ReportTask(void *argument)
{
    (void)argument;

    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/devstatus",
             s_ah_mqtt.topic_info.station_hex,
             s_ah_mqtt.topic_info.lane_hex,
             s_ah_mqtt.topic_info.device_type,
             s_ah_mqtt.topic_info.device_id);

    static state_report_t report = {
        .work_sta = '0',
        .reserved = "00000000",
    };

    for (;;) {
        memcpy(&(report.notify), &s_ah_mqtt.notify_id, sizeof(report.notify));
        report.run_sta = dev_display_get()->light_level ? '1' : '0'; // 根据当前亮度判断是否开显示屏
        /* 长度显式给出：保持原有 strlen 语义（结构体内含 NUL 时按字符串截断） */
        mqtt_send_data(topic, &report, (uint16_t)strlen((char *)&report));
        osDelay(10 * 1000); // 10秒上报1次
    }
}

void SignUpTask(void *argument)
{
    (void)argument;

    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/sign",
             s_ah_mqtt.topic_info.station_hex,
             s_ah_mqtt.topic_info.lane_hex,
             s_ah_mqtt.topic_info.device_type,
             s_ah_mqtt.topic_info.device_id);

    static sign_up_t sign_up = {
        .type         = '1',
        .work_sta     = '0',
        .soft_ver     = "SoftV1.0.0",
        .hard_ver     = "FirmwareV1",
        .protocol_ver = "LEDV1.0.00",
        .company      = "CQChuangDi",
        .device       = "LEDScreen1",
        .reserved     = "00000000000000000000",
    };
    memcpy(&(sign_up.notify), &s_ah_mqtt.notify_id, sizeof(sign_up.notify));
    mqtt_send_data(topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    sign_up.type = '0';

    for (;;) {
        osDelay(5 * 60 * 1000); // 5分钟签到1次
        memcpy(&(sign_up.notify), &s_ah_mqtt.notify_id, sizeof(sign_up.notify));
        mqtt_send_data(topic, &sign_up, (uint16_t)strlen((char *)&sign_up));
    }
}

/* ---- 自注册到 app_dispatch ---- */
[[maybe_unused]] static void ah_mqtt_module_init(void)
{
    rb_init(&s_ah_mqtt_rb, "ah_mqtt");

    /* 队列在此建好：通道任务可能早于协议任务首次运行就投递帧 */
    g_proto_ah_matt_queue = osMessageQueueNew(1, AH_MQTT_MSG_SIZE, &s_ah_mqtt_queue_attr);
    s_ah_mqtt.base.queue  = g_proto_ah_matt_queue;

    /* 订阅哪些主题是协议知识：登记一次，通道在每次连接（含重连）就绪时施加 */
    for (uint8_t i = 0; i < AH_MQTT_CMD_COUNT; i++)
        s_sub_topics[i] = g_ah_mqtt_cmd_table[i].topic;
    app_mqtt_subscribe(s_sub_topics, AH_MQTT_CMD_COUNT);

    app_proto_bind(&s_ah_mqtt.base, app_mqtt_ccb());

    // 创建协议处理任务
    g_ah_mqtt_task_handle = pl_task_new(ah_mqtt_handle_task, NULL, &ProtocolTask_attributes);
}
/* 保持注释：本模块当前不启用（app_mqtt_start 亦无调用者，MQTT 链路未激活）。 */
// sw_app_initcall(ah_mqtt_module_init);
