#include "ah_mqtt_cmd.h"

#include "ah_mqtt.h"
#include "text_cvt.h"
#include "app_screen.h"
#include "app_render.h"
#include "dev_display.h"

#include <string.h>

/* 命令处理一律回传协议对象：自身状态（设备标识、通知号、回复主题）经 base 取回，
   不再依赖文件级全局，也不必认识具体通道类型（发送走 ccb_send_to 虚表）。 */
static ah_mqtt_proto_t *proto_of(pcb_t *self)
{
    return container_of(self, ah_mqtt_proto_t, base);
}

static void reply_send(pcb_t *self, const char *suffix);

static void cmd_SendData(pcb_t *self, ccb_t *ccb, uint32_t ReturnLen, char *ReturnData);

static void cmd_display(pcb_t *self, ccb_t *ccb, char *buff);
static void cmd_fill(pcb_t *self, ccb_t *ccb, char *buff);
static void cmd_restart(pcb_t *self, ccb_t *ccb, char *buff);
static void cmd_checktime(pcb_t *self, ccb_t *ccb, char *buff);

/* 主题与处理函数同表：索引即命令号，不会漂移。未列出的主题不属于本协议，
   探针会直接整帧跳过（原 cmd_default 桩因此不再需要）。 */
const ah_mqtt_cmd_entry_t g_ah_mqtt_cmd_table[AH_MQTT_CMD_COUNT] = {
    {"ASK/board/NULL", cmd_display},
    {"ASK/display/clean", cmd_fill},
    {"ASK/op/restart", cmd_restart},
    {"ASK/op/checktime", cmd_checktime},
};

const static display_color_t disp_color[] = {COLOR_BLACK, COLOR_GREEN, COLOR_RED, COLOR_YELLOW};
const static font_size_t disp_size[]    = {0, 0, FONT_16, FONT_24, FONT_32, 0, 0, 0, FONT_32};
/**
 * @brief 显示文字
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void cmd_display(pcb_t *self, ccb_t *ccb, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    cmd_display_t *para = (cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    UTF8ToGBK(para->text, length - sizeof(cmd_display_t), gbk_txt, &gbk_len);
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT,
        .x = 0, .y = 0,
        .w = app_screen_rows(), .h = app_screen_cols(),
        .color    = disp_color[(uint8_t)(para->color) - 0x30],
        .text      = gbk_txt, .len = (uint16_t)gbk_len,
        .font_size = disp_size[(uint8_t)(para->size) - 0x30],
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_UTF8,
    });

    // 更新本地的notify id
    memcpy(&proto_of(self)->notify_id, &(para->nid), sizeof(notify_id_t));

    reply_send(self, "Reply/board/NULL");
    cmd_SendData(self, ccb, sizeof("0"), "0");
}

/**
 * @brief 全屏填充
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void cmd_fill(pcb_t *self, ccb_t *ccb, char *buff)
{
    cmd_fill_t *para = (cmd_fill_t *)buff;

    app_render(&(render_cfg_t){.type = RENDER_FILL, .color = disp_color[(uint8_t)(para->color) - 0x30]});

    reply_send(self, "Reply/display/clean");
    cmd_SendData(self, ccb, sizeof("0"), "0");
}

/**
 * @brief 重启
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void cmd_restart(pcb_t *self, ccb_t *ccb, char *buff)
{
    (void)buff;

    reply_send(self, "Reply/op/restart");
    cmd_SendData(self, ccb, sizeof("0"), "0");

    NVIC_SystemReset();
}

/**
 * @brief 对时
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void cmd_checktime(pcb_t *self, ccb_t *ccb, char *buff)
{
    cmd_checktime_t *para = (cmd_checktime_t *)buff;
    notify_date_t *date   = (notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(proto_of(self)->notify_id.date_time), date, sizeof(notify_date_t));

    reply_send(self, "Reply/op/checktime");
    cmd_SendData(self, ccb, sizeof("0"), "0");
}

/**
 * @brief 拼接回复主题（<设备标识>/<后缀>）并记入协议对象，供随后的 ccb_send_to 使用
 *
 * @param self   协议对象（提供设备标识与回复缓冲）
 * @param suffix 命令对应的回复后缀
 */
static void reply_send(pcb_t *self, const char *suffix)
{
    ah_mqtt_proto_t    *p = proto_of(self);
    const topic_info_t *t = &p->topic_info;

    /* 写进协议自有缓冲，随后随消息交给通道 —— 不再借用通道对象的主题缓冲区
       （那是通道的接收状态，改它等于把手伸进别人的对象） */
    snprintf(p->reply_topic, sizeof(p->reply_topic), "%.8s/%.2s/%.2s/%.2s/%s",
             t->station_hex, t->lane_hex, t->device_type, t->device_id, suffix);
}

/**
 * @brief 内部函数，封装了按协议格式发送消息（通知号头 + 数据）
 *
 * 发送长度沿用原有的 strlen 语义：净荷首部是通知号（全为可打印字符），
 * 其后是数据，故 strlen 恰为「通知号长度 + 数据字符串长度」。
 *
 * @param self 协议对象（提供通知号）
 * @param ccb 通道实例
 * @param ReturnLen 消息长度
 * @param ReturnData 发布的消息
 */
static void cmd_SendData(pcb_t *self, ccb_t *ccb, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    ah_mqtt_proto_t *p = proto_of(self);

    memcpy(ReturnBuff, &p->notify_id, sizeof(notify_id_t));
    memcpy(&ReturnBuff[sizeof(notify_id_t)], ReturnData, ReturnLen);

    /* 目的地随消息表达：回复主题由协议给出，通道只负责翻译成自己的机制 */
    const ccb_dst_t dst = {.topic = p->reply_topic};
    ccb_send_to(ccb, &dst, (const uint8_t *)ReturnBuff, (uint16_t)strlen(ReturnBuff));
}
