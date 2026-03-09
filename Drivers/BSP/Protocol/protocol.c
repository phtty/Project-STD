#include "protocol.h"

#include "mqtt_app.h"
#include "cmd.h"
#include "display.h"

static uint8_t ptcl_buff[FRAME_MAX_LEN] = {0};

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

osThreadId_t ProtocolHandle;
const osThreadAttr_t ProtocolTask_attributes = {
    .name       = "ProtocolTask",
    .stack_size = 256 * 4,
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

/**
 * @brief 协议解析任务
 *
 */
void ProtocolTask(void *argument)
{
    uint16_t length = 0;

    // 创建两个任务，用于定时上报状态和签到
    SignUpHandle = osThreadNew(SignUpTask, NULL, &SignUpTask_attributes);
    ReportHandle = osThreadNew(ReportTask, NULL, &ReportTask_attributes);

    for (;;) {
        osMessageQueueGet(xMessageLenQueue, &length, NULL, osWaitForever);

        BSP_RB_GetByte_Bulk(&xProtocol_RB, ptcl_buff, length);
        pfCmdFunc[ucCmdCmdIndex]((char *)ptcl_buff);
    }
}

/**
 * @brief 解析topic确认是哪个命令
 *
 */
void handle_topic(const char topic[])
{
    if (NULL != strstr(topic, "/ASK/board/NULL"))
        ucCmdCmdIndex = 1;

    else if (NULL != strstr(topic, "/ASK/display/clean"))
        ucCmdCmdIndex = 2;

    else if (NULL != strstr(topic, "/ASK/op/restart"))
        ucCmdCmdIndex = 3;

    else if (NULL != strstr(topic, "/ASK/op/checktime"))
        ucCmdCmdIndex = 4;

    else
        ucCmdCmdIndex = 0;
}

/**
 * @brief 10秒上报状态任务
 *
 * @param argument
 */
void ReportTask(void *argument)
{
    static char topic[64] = {0};
    snprintf(topic, 64, "%s/%s/%s/%s/Push/monitor/devstatus",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);

    static state_report_t report = {
        .work_sta = '0',
        .time     = "00000000000000",
        .reserved = "00000000",
    };

    for (;;) {
        memcpy(&(report.notify), &xNotifyID, sizeof(report.notify));
        report.run_sta = light_level ? '1' : '0'; // 根据当前亮度判断是否开显示屏
        mqtt_send_data(topic, (char *)&report);
        osDelay(10 * 1000); // 10秒签到1次
    }
}

/**
 * @brief 5分钟签到任务
 *
 * @param argument
 */
void SignUpTask(void *argument)
{
    static char topic[64] = {0};
    snprintf(topic, 64, "%s/%s/%s/%s/Push/monitor/sign",
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
