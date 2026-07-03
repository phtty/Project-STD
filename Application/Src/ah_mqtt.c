#include "ah_mqtt.h"

#include "ah_mqtt_cmd.h"
#include "dev_display.h"
#include "app_mqtt.h"
#include "initcall.h"

osMessageQueueId_t g_proto_ah_matt_queue;

topic_info_t topic_info = {
    .station_hex = "11451419",
    .lane_hex    = "01",
    .device_type = "32",
    .device_id   = "01",
};

notify_date_t notify_date = {
    .year  = "2025",
    .month = "03",
    .day   = "05",
    .hour  = "14",
};

notify_id_t xNotifyID = {
    .date_time = {
        .year  = "2025",
        .month = "03",
        .day   = "05",
        .hour  = "14",
    },
    .device_type = "32",
    .device_id   = "01",
    .send_count  = "000000",
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
static uint8_t handle_topic(const char topic[]);

void ah_mqtt_handle_task(void *argument)
{
    frame_msg_t msg;
    const osMessageQueueAttr_t proto_ah_mqtt_queue_attr = {
        .name = "g_proto_ah_matt_queue",
    };
    g_proto_ah_matt_queue = osMessageQueueNew(1, sizeof(frame_msg_t), &proto_ah_mqtt_queue_attr);
    app_proto_set_frame_queue(PROTO_MASK_AH_MQTT, g_proto_ah_matt_queue);

    while (g_mqtt.state != MQTT_ST_READY) { // 等待连接建立
        osDelay(100);
    }
    // 创建两个任务，用于定时上报状态和签到
    SignUpHandle = osThreadNew(SignUpTask, NULL, &SignUpTask_attributes);
    ReportHandle = osThreadNew(ReportTask, NULL, &ReportTask_attributes);

    for (;;) {
        // 等待多协议多信道模块分发数据帧
        if (osOK != osMessageQueueGet(g_proto_ah_matt_queue, &msg, NULL, osWaitForever)) {
            continue;
        }

        // 通过topic得知是哪个命令
        uint16_t cmd = handle_topic(container_of(msg.ch, mqtt_channel_t, me)->topic);

        g_ah_mqtt_cmd_table[cmd](msg.ch, (char *)(msg.data));
    }
}

/**
 * @brief 解析topic确认是哪个命令
 *
 */
uint8_t handle_topic(const char topic[])
{
    if (NULL != strstr(topic, "/ASK/board/NULL"))
        return 1;

    else if (NULL != strstr(topic, "/ASK/display/clean"))
        return 2;

    else if (NULL != strstr(topic, "/ASK/op/restart"))
        return 3;

    else if (NULL != strstr(topic, "/ASK/op/checktime"))
        return 4;

    else
        return 0;
}

proto_probe_sta_t ah_mqtt_probe_frame(const channel_t *ch, const ring_buffer_t *buff, uint32_t *payload_len, uint8_t *cmd_num)
{
    (void)buff;
    *payload_len = container_of(ch, mqtt_channel_t, me)->payload_len;
    *cmd_num     = handle_topic(container_of(ch, mqtt_channel_t, me)->topic);

    return PROTO_PROBE_READY;
}

void ReportTask(void *argument)
{
    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/devstatus",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);

    static state_report_t report = {
        .work_sta = '0',
        .reserved = "00000000",
    };

    for (;;) {
        memcpy(&(report.notify), &xNotifyID, sizeof(report.notify));
        report.run_sta = dev_display_get()->light_level ? '1' : '0'; // 根据当前亮度判断是否开显示屏
        mqtt_send_data(topic, (char *)&report);
        osDelay(10 * 1000); // 10秒签到1次
    }
}

void SignUpTask(void *argument)
{
    static char topic[64] = {0};
    snprintf(topic, 64, "%.8s/%.2s/%.2s/%.2s/Push/monitor/sign",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);

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
    memcpy(&(sign_up.notify), &xNotifyID, sizeof(sign_up.notify));
    mqtt_send_data(topic, (char *)&sign_up);
    sign_up.type = '0';

    for (;;) {
        osDelay(5 * 60 * 1000); // 5分钟签到1次
        memcpy(&(sign_up.notify), &xNotifyID, sizeof(sign_up.notify));
        mqtt_send_data(topic, (char *)&sign_up);
    }
}

/* ---- 自注册到 app_dispatch ---- */
[[maybe_unused]] static void ah_mqtt_module_init(void)
{
    ring_buffer_t *rb = app_proto_acquire_buf(1, 2048);
    app_proto_register(PROTO_MASK_AH_MQTT, ah_mqtt_probe_frame, rb);
    app_proto_bind_channel(PROTO_MASK_AH_MQTT, CH_ID_MQTT);

    // 创建协议处理任务
    g_ah_mqtt_task_handle = osThreadNew(ah_mqtt_handle_task, NULL, &ProtocolTask_attributes);
}
// sw_app_initcall(ah_mqtt_module_init);
