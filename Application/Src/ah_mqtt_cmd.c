#include "ah_mqtt_cmd.h"

#include "app_mqtt.h"
#include "ah_mqtt.h"
#include "text_cvt.h"
#include "app_render.h"
#include "dev_display.h"

static void cmd_SendData(channel_t *ch, uint32_t ReturnLen, char *ReturnData);
static void cmd_default(channel_t *ch, char *buff);
static void cmd_display(channel_t *ch, char *buff);
static void cmd_fill(channel_t *ch, char *buff);
static void cmd_restart(channel_t *ch, char *buff);
static void cmd_checktime(channel_t *ch, char *buff);

const ah_mqtt_cmd_handler_fn_t g_ah_mqtt_cmd_table[] = {
    cmd_default,
    cmd_display,
    cmd_fill,
    cmd_restart,
    cmd_checktime,
};

/**
 * @brief 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷
 *
 * @param meta mqtt锟脚碉拷元锟斤拷锟斤拷
 * @param buff 锟斤拷锟斤拷锟斤拷锟
 */
static void cmd_default(channel_t *ch, char *buff)
{
}

const static hub75_color_t disp_color[] = {HUB75_COLOR_BLACK, HUB75_COLOR_GREEN, HUB75_COLOR_RED, HUB75_COLOR_YELLOW};
const static font_size_t disp_size[]    = {0, 0, FONT_SIZE_16, FONT_SIZE_24, FONT_SIZE_32, 0, 0, 0, FONT_SIZE_32};
/**
 * @brief 锟斤拷示锟斤拷锟斤拷
 *
 * @param meta mqtt锟脚碉拷元锟斤拷锟斤拷
 * @param buff 锟斤拷锟斤拷锟斤拷锟
 */
static void cmd_display(channel_t *ch, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    app_render_string(dev_display_get(), NULL, 0, 0, gbk_txt, gbk_len, disp_color[(uint8_t)(para->color) - 0x30], disp_size[(uint8_t)(para->size) - 0x30]);

    // 锟斤拷锟铰憋拷锟截碉拷notify id
    memcpy(&xNotifyID, &(para->nid), sizeof(xNotifyID));

    snprintf(container_of(ch, mqtt_channel_t, ch)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/board/NULL",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief 全锟斤拷锟斤拷锟
 *
 * @param meta mqtt锟脚碉拷元锟斤拷锟斤拷
 * @param buff 锟斤拷锟斤拷锟斤拷锟
 */
static void cmd_fill(channel_t *ch, char *buff)
{
    // uint32_t length  = strlen(buff);
    cmd_fill_t *para = (cmd_fill_t *)buff;

    app_render_fill(dev_display_get(), disp_color[(uint8_t)(para->color) - 0x30]);

    snprintf(container_of(ch, mqtt_channel_t, ch)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/display/clean",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief 锟斤拷锟斤拷
 *
 * @param meta mqtt锟脚碉拷元锟斤拷锟斤拷
 * @param buff 锟斤拷锟斤拷锟斤拷锟
 */
static void cmd_restart(channel_t *ch, char *buff)
{
    snprintf(container_of(ch, mqtt_channel_t, ch)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/restart",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");

    NVIC_SystemReset(); // 锟斤拷锟斤拷
}

/**
 * @brief 锟斤拷时
 *
 * @param meta mqtt锟脚碉拷元锟斤拷锟斤拷
 * @param buff 锟斤拷锟斤拷锟斤拷锟
 */
static void cmd_checktime(channel_t *ch, char *buff)
{
    cmd_checktime_t *para = (cmd_checktime_t *)buff;
    notify_date_t *date   = (notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(xNotifyID.date_time), date, sizeof(notify_date_t));

    snprintf(container_of(ch, mqtt_channel_t, ch)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/checktime",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief  锟节诧拷锟斤拷锟斤拷锟斤拷锟斤拷装锟剿帮拷协锟斤拷锟绞斤拷锟斤拷锟斤拷锟较
 *
 * @param  meta mqtt锟脚碉拷元锟斤拷锟捷ｏ拷锟斤拷topic锟斤拷息
 * @param  ReturnLen 锟斤拷息锟斤拷锟斤拷
 * @param  ReturnData 锟斤拷锟斤拷锟斤拷锟斤拷息
 */
static void cmd_SendData(channel_t *ch, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    memcpy(ReturnBuff, &xNotifyID, sizeof(xNotifyID));
    memcpy(&ReturnBuff[sizeof(xNotifyID)], ReturnData, ReturnLen);

    channel_send(ch, (uint8_t *)ReturnBuff, strlen(ReturnBuff));
}
