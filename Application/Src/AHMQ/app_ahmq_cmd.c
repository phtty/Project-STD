#include "app_ahmq_cmd.h"

#include "app_ahmq.h"
#include "text_cvt.h"
#include "app_screen.h"
#include "app_render.h"
#include "dev_display.h"

#include <string.h>

/* 命令处理一律回传协议对象：自身状态（设备标识、通知号、回复主题）经 base 取回，
   不再依赖文件级全局，也不必认识具体通道类型（发送走 app_ccb_send_to 虚表）。 */
static app_ahmq_proto_t *proto_of(app_pcb_t *self)
{
    return container_of(self, app_ahmq_proto_t, base);
}

static void reply_send(app_pcb_t *self, const char *suffix);

static void cmd_SendData(app_pcb_t *self, app_ccb_t *ccb, uint32_t ReturnLen, char *ReturnData);

static void _cmd_display(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_fill(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_restart(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_checktime(app_pcb_t *self, app_ccb_t *ccb, char *buff);

/* 主题与处理函数同表：索引即命令号，不会漂移。未列出的主题不属于本协议，
   探针会直接整帧跳过（原 cmd_default 桩因此不再需要）。 */
const app_ahmq_cmd_entry_t g_ahmq_cmd_table[AHMQ_CMD_COUNT] = {
    {"ASK/board/NULL", _cmd_display},
    {"ASK/display/clean", _cmd_fill},
    {"ASK/op/restart", _cmd_restart},
    {"ASK/op/checktime", _cmd_checktime},
};

const static dev_display_color_t disp_color[] = {DEV_DISPLAY_COLOR_BLACK, DEV_DISPLAY_COLOR_GREEN, DEV_DISPLAY_COLOR_RED, DEV_DISPLAY_COLOR_YELLOW};
const static app_font_size_t disp_size[]    = {0, 0, APP_FONT_SIZE_16, APP_FONT_SIZE_24, APP_FONT_SIZE_32, 0, 0, 0, APP_FONT_SIZE_32};
/**
 * @brief 显示文字
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void _cmd_display(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    uint32_t length     = strlen(buff), gbk_len;
    app_ahmq_cmd_display_t *para = (app_ahmq_cmd_display_t *)buff;
    char gbk_txt[128]   = {0};

    cvt_utf8_to_gbk(para->text, length - sizeof(app_ahmq_cmd_display_t), gbk_txt, &gbk_len);
    app_render(&(app_render_cfg_t){
        .type = APP_RENDER_TYPE_TEXT,
        .x = 0, .y = 0,
        .w = app_screen_rows(), .h = app_screen_cols(),
        .color    = disp_color[(uint8_t)(para->color) - 0x30],
        .text      = gbk_txt, .len = (uint16_t)gbk_len,
        .font_size = disp_size[(uint8_t)(para->size) - 0x30],
        .font_type = APP_FONT_TYPE_ST,
        .text_enc  = APP_FONT_ENC_UTF8,
    });

    // 更新本地的notify id
    memcpy(&proto_of(self)->notify_id, &(para->nid), sizeof(app_ahmq_notify_id_t));

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
static void _cmd_fill(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    app_ahmq_cmd_fill_t *para = (app_ahmq_cmd_fill_t *)buff;

    app_render(&(app_render_cfg_t){.type = APP_RENDER_TYPE_FILL, .color = disp_color[(uint8_t)(para->color) - 0x30]});

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
static void _cmd_restart(app_pcb_t *self, app_ccb_t *ccb, char *buff)
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
static void _cmd_checktime(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    app_ahmq_cmd_checktime_t *para = (app_ahmq_cmd_checktime_t *)buff;
    app_ahmq_notify_date_t *date   = (app_ahmq_notify_date_t *)(para->time);

    if (para->type == '1')
        memcpy(&(proto_of(self)->notify_id.date_time), date, sizeof(app_ahmq_notify_date_t));

    reply_send(self, "Reply/op/checktime");
    cmd_SendData(self, ccb, sizeof("0"), "0");
}

/**
 * @brief 拼接回复主题（设备标识 + 后缀）并记入协议对象，供随后的 app_ccb_send_to 使用
 *
 * @param self   协议对象（提供设备标识与回复缓冲）
 * @param suffix 命令对应的回复后缀
 */
static void reply_send(app_pcb_t *self, const char *suffix)
{
    app_ahmq_proto_t    *p = proto_of(self);
    const app_ahmq_topic_info_t *t = &p->topic_info;

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
static void cmd_SendData(app_pcb_t *self, app_ccb_t *ccb, uint32_t ReturnLen, char *ReturnData)
{
    char ReturnBuff[128] = {0};

    app_ahmq_proto_t *p = proto_of(self);

    memcpy(ReturnBuff, &p->notify_id, sizeof(app_ahmq_notify_id_t));
    memcpy(&ReturnBuff[sizeof(app_ahmq_notify_id_t)], ReturnData, ReturnLen);

    /* 目的地随消息表达：回复主题由协议给出，通道只负责翻译成自己的机制 */
    const app_ccb_dst_t dst = {.topic = p->reply_topic};
    app_ccb_send_to(ccb, &dst, (const uint8_t *)ReturnBuff, (uint16_t)strlen(ReturnBuff));
}
