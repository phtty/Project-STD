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
 * @brief 测试用命令
 *
 * @param ch 通道实例
 * @param buff 命令参数
 */
static void cmd_default(channel_t *ch, char *buff)
{
}

const static display_color_t disp_color[] = {COLOR_BLACK, COLOR_GREEN, COLOR_RED, COLOR_YELLOW};
const static font_size_t disp_size[]    = {0, 0, FONT_16, FONT_24, FONT_32, 0, 0, 0, FONT_32};
/**
 * @brief 显示文字
 *
 * @param ch 通道实例
 * @param buff 命令参数
 */
static void cmd_display(channel_t *ch, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT,
        .x = 0, .y = 0,
        .w = dev_display_get()->screen_rows, .h = dev_display_get()->screen_cols,
        .color    = disp_color[(uint8_t)(para->color) - 0x30],
        .text      = gbk_txt, .len = (uint16_t)gbk_len,
        .font_size = disp_size[(uint8_t)(para->size) - 0x30],
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_UTF8,
    });

    // 更新本地的notify id
    memcpy(&xNotifyID, &(para->nid), sizeof(xNotifyID));

    snprintf(container_of(ch, mqtt_channel_t, me)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/board/NULL",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief 全屏填充
 *
 * @param ch 通道实例
 * @param buff 命令参数
 */
static void cmd_fill(channel_t *ch, char *buff)
{
    // uint32_t length  = strlen(buff);
    cmd_fill_t *para = (cmd_fill_t *)buff;

    app_render(&(render_cfg_t){.type = RENDER_FILL, .color = disp_color[(uint8_t)(para->color) - 0x30]});

    snprintf(container_of(ch, mqtt_channel_t, me)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/display/clean",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief 重启
 *
 * @param ch 通道实例
 * @param buff 命令参数
 */
static void cmd_restart(channel_t *ch, char *buff)
{
    snprintf(container_of(ch, mqtt_channel_t, me)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/restart",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");

    NVIC_SystemReset(); // 锟斤拷锟斤拷
}

/**
 * @brief 对时
 *
 * @param ch 通道实例
 * @param buff 命令参数
 */
static void cmd_checktime(channel_t *ch, char *buff)
{
    cmd_checktime_t *para = (cmd_checktime_t *)buff;
    notify_date_t *date   = (notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(xNotifyID.date_time), date, sizeof(notify_date_t));

    snprintf(container_of(ch, mqtt_channel_t, me)->topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/checktime",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(ch, sizeof("0"), "0");
}

/**
 * @brief 内部函数，封装了按协议格式发送消息
 *
 * @param ch 通道实例
 * @param ReturnLen 消息长度
 * @param ReturnData 发布的消息
 */
static void cmd_SendData(channel_t *ch, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    memcpy(ReturnBuff, &xNotifyID, sizeof(xNotifyID));
    memcpy(&ReturnBuff[sizeof(xNotifyID)], ReturnData, ReturnLen);

    channel_send(ch, (uint8_t *)ReturnBuff, strlen(ReturnBuff));
}
