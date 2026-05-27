#include "ah_mqtt_cmd.h"

#include "mqtt_app.h"
#include "ah_mqtt.h"
#include "text_cvt.h"
#include "app_render.h"
#include "dev_display.h"

static void cmd_SendData(ch_meta_t *meta, uint32_t ReturnLen, char *ReturnData);
static void cmd_default(ch_meta_t *meta, char *buff);
static void cmd_display(ch_meta_t *meta, char *buff);
static void cmd_fill(ch_meta_t *meta, char *buff);
static void cmd_restart(ch_meta_t *meta, char *buff);
static void cmd_checktime(ch_meta_t *meta, char *buff);

const ah_mqtt_cmd_handler_fn_t g_ah_mqtt_cmd_table[] = {
    cmd_default,
    cmd_display,
    cmd_fill,
    cmd_restart,
    cmd_checktime,
};

/**
 * @brief ����������
 *
 * @param meta mqtt�ŵ�Ԫ����
 * @param buff �������
 */
static void cmd_default(ch_meta_t *meta, char *buff)
{
}

const static hub75_color_t disp_color[] = {HUB75_COLOR_BLACK, HUB75_COLOR_GREEN, HUB75_COLOR_RED, HUB75_COLOR_YELLOW};
const static font_size_t   disp_size[]   = {0, 0, FONT_SIZE_16, FONT_SIZE_24, FONT_SIZE_32, 0, 0, 0, FONT_SIZE_32};
/**
 * @brief ��ʾ����
 *
 * @param meta mqtt�ŵ�Ԫ����
 * @param buff �������
 */
static void cmd_display(ch_meta_t *meta, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    app_render_string(dev_display_get(), NULL, 0, 0, gbk_txt, gbk_len, disp_color[(uint8_t)(para->color) - 0x30], disp_size[(uint8_t)(para->size) - 0x30]);

    // ���±��ص�notify id
    memcpy(&xNotifyID, &(para->nid), sizeof(xNotifyID));

    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/board/NULL",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");
}

/**
 * @brief ȫ�����
 *
 * @param meta mqtt�ŵ�Ԫ����
 * @param buff �������
 */
static void cmd_fill(ch_meta_t *meta, char *buff)
{
    // uint32_t length  = strlen(buff);
    cmd_fill_t *para = (cmd_fill_t *)buff;

    app_render_fill(dev_display_get(), disp_color[(uint8_t)(para->color) - 0x30]);

    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/display/clean",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");
}

/**
 * @brief ����
 *
 * @param meta mqtt�ŵ�Ԫ����
 * @param buff �������
 */
static void cmd_restart(ch_meta_t *meta, char *buff)
{
    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/restart",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");

    NVIC_SystemReset(); // ����
}

/**
 * @brief ��ʱ
 *
 * @param meta mqtt�ŵ�Ԫ����
 * @param buff �������
 */
static void cmd_checktime(ch_meta_t *meta, char *buff)
{
    cmd_checktime_t *para = (cmd_checktime_t *)buff;
    notify_date_t *date   = (notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(xNotifyID.date_time), date, sizeof(notify_date_t));

    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/checktime",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");
}

/**
 * @brief  �ڲ���������װ�˰�Э���ʽ������Ϣ
 *
 * @param  meta mqtt�ŵ�Ԫ���ݣ���topic��Ϣ
 * @param  ReturnLen ��Ϣ����
 * @param  ReturnData ��������Ϣ
 */
static void cmd_SendData(ch_meta_t *meta, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    memcpy(ReturnBuff, &xNotifyID, sizeof(xNotifyID));
    memcpy(&ReturnBuff[sizeof(xNotifyID)], ReturnData, ReturnLen);

    channel_send(meta, (uint8_t *)ReturnBuff, strlen(ReturnBuff));
}
