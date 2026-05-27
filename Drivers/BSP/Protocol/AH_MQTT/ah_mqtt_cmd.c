#include "ah_mqtt_cmd.h"

#include "mqtt_app.h"
#include "ah_mqtt.h"
#include "text_cvt.h"
#include "render.h"

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
 * @brief 测试用命令
 *
 * @param meta mqtt信道元数据
 * @param buff 命令参数
 */
static void cmd_default(ch_meta_t *meta, char *buff)
{
}

const static DispColor_t disp_color[] = {black, green, red, yellow};
const static FontSize_t disp_size[]   = {0, 0, font_16, font_24, font_32, 0, 0, 0, font_32};
/**
 * @brief 显示文字
 *
 * @param meta mqtt信道元数据
 * @param buff 命令参数
 */
static void cmd_display(ch_meta_t *meta, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    RenderString(0, 0, gbk_txt, gbk_len, disp_color[(uint8_t)(para->color) - 0x30], disp_size[(uint8_t)(para->size) - 0x30], font_ht);

    // 更新本地的notify id
    memcpy(&xNotifyID, &(para->nid), sizeof(xNotifyID));

    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/board/NULL",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");
}

/**
 * @brief 全屏填充
 *
 * @param meta mqtt信道元数据
 * @param buff 命令参数
 */
static void cmd_fill(ch_meta_t *meta, char *buff)
{
    // uint32_t length  = strlen(buff);
    cmd_fill_t *para = (cmd_fill_t *)buff;

    Disp_Fill(disp_color[(uint8_t)(para->color) - 0x30]);

    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/display/clean",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");
}

/**
 * @brief 重启
 *
 * @param meta mqtt信道元数据
 * @param buff 命令参数
 */
static void cmd_restart(ch_meta_t *meta, char *buff)
{
    snprintf(meta->handle.mqtt.topic, 64, "%.8s/%.2s/%.2s/%.2s/Reply/op/restart",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(meta, sizeof("0"), "0");

    NVIC_SystemReset(); // 重启
}

/**
 * @brief 对时
 *
 * @param meta mqtt信道元数据
 * @param buff 命令参数
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
 * @brief  内部函数，封装了按协议格式发送消息
 *
 * @param  meta mqtt信道元数据，有topic信息
 * @param  ReturnLen 消息长度
 * @param  ReturnData 发布的消息
 */
static void cmd_SendData(ch_meta_t *meta, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    memcpy(ReturnBuff, &xNotifyID, sizeof(xNotifyID));
    memcpy(&ReturnBuff[sizeof(xNotifyID)], ReturnData, ReturnLen);

    channel_send(meta, (uint8_t *)ReturnBuff, strlen(ReturnBuff));
}
