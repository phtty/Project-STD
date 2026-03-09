#include "cmd.h"

#include "mqtt_app.h"
#include "protocol.h"
#include "text_cvt.h"
#include "render.h"

uint8_t ucCmdCmdIndex   = 0;
uint32_t ulCmdSendCount = 0;

static void cmd_SendData(char topic[], uint32_t ReturnLen, char *ReturnData);
static void cmd_default(char *buff);
static void cmd_display(char *buff);
static void cmd_fill(char *buff);
static void cmd_restart(char *buff);
static void cmd_checktime(char *buff);

const CmdFunc_t pfCmdFunc[] = {
    cmd_default,
    cmd_display,
    cmd_fill,
    cmd_restart,
    cmd_checktime,
};

/**
 * @brief 测试用命令
 *
 * @param buff 命令参数
 */
static void cmd_default(char *buff)
{
}

const static DispColor_t disp_color[] = {black, green, red, yellow};
const static FontSize_t disp_size[]   = {0, 0, font_16, font_24, font_32, 0, 0, 0, font_32};
/**
 * @brief 显示文字
 *
 * @param buff 命令参数
 */
static void cmd_display(char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    RenderString(0, 0, gbk_txt, gbk_len, disp_color[(uint8_t)(para->color) - 0x30], disp_size[(uint8_t)(para->size) - 0x30], font_ht);

    // 更新本地的notify id
    memcpy(&xNotifyID, &(para->nid), sizeof(xNotifyID));

    char temp[64] = {0};
    snprintf(temp, 64, "%s/%s/%s/%s/Reply/board/NULL",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(temp, sizeof("0"), "0");
}

/**
 * @brief 全屏填充
 *
 * @param buff 命令参数
 */
static void cmd_fill(char *buff)
{
    // uint32_t length  = strlen(buff);
    cmd_fill_t *para = (cmd_fill_t *)buff;

    Disp_Fill(para->color);

    char temp[64] = {0};
    snprintf(temp, 64, "%s/%s/%s/%s/Reply/display/clean",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(temp, sizeof("0"), "0");
}

/**
 * @brief 重启
 *
 * @param buff 命令参数
 */
static void cmd_restart(char *buff)
{
    // uint32_t length     = strlen(buff);
    // cmd_restart_t *para = (cmd_restart_t *)buff;

    char temp[64] = {0};
    snprintf(temp, 64, "%s/%s/%s/%s/Reply/op/restart",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(temp, sizeof("0"), "0");

    NVIC_SystemReset(); // 重启
}

/**
 * @brief 对时
 *
 * @param buff 命令参数
 */
static void cmd_checktime(char *buff)
{
    // uint32_t length     = strlen(buff);
    cmd_checktime_t *para = (cmd_checktime_t *)buff;
    notify_date_t *date   = (notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(xNotifyID.date_time), date, sizeof(notify_date_t));

    char temp[64] = {0};
    snprintf(temp, 64, "%s/%s/%s/%s/Reply/op/checktime",
             topic_info.station_hex,
             topic_info.lane_hex,
             topic_info.device_type,
             topic_info.device_id);
    cmd_SendData(temp, sizeof("0"), "0");
}

/**
 * @brief  内部函数，封装了按协议格式发送消息
 *
 * @param  topic 要发布的主题
 * @param  ReturnLen 消息长度
 * @param  ReturnData 发布的消息
 */
static void cmd_SendData(char topic[], uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    memcpy(ReturnBuff, &xNotifyID, sizeof(xNotifyID));
    memcpy(&ReturnBuff[sizeof(xNotifyID)], ReturnData, ReturnLen);

    mqtt_send_data(topic, ReturnBuff);
}
