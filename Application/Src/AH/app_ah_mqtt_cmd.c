/**
 * @file    app_ah_mqtt_cmd.c
 * @brief   AH/MQTT 命令处理实现：按命令表分派到显示/填充/重启/对时
 *
 * 命令表只存与设备身份无关的**层级后缀**；完整订阅主题由 `app_ah_mqtt` 用配置拼出。
 * 显示命令按 `cfg.device_id` 在运行时分派（32H 情报板本期实现，31H 只记日志），
 * 不在编译期写死设备类型。
 */

#include "app_ah_mqtt_cmd.h"

#include "app_ah_mqtt.h"
#include "app_render.h"
#include "app_screen.h"
#include "dev_display.h"
#include "pl_rtc.h"
#include "text_cvt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

/* 命令处理一律回传协议对象：自身状态（主题前缀、通知号、回复主题）经 base 取回，
   不再依赖文件级全局，也不必认识具体通道类型（发送走 app_ccb_send_to 虚表）。 */
static app_ah_mqtt_proto_t *_ah_mqtt_proto_of(app_pcb_t *self)
{
    return container_of(self, app_ah_mqtt_proto_t, base);
}

static void _ah_mqtt_reply_send(app_pcb_t *self, const char *suffix);

/** @brief 把请求里的通知号抄进协议对象；载荷过短时只抄实际有的字节，其余补 0
 *
 *  MQTT 通道只保证“以 NUL 结尾”，不保证业务载荷长度；短帧不能越界读，
 *  也不能拿上一帧的旧号回复。
 */
static void _ah_mqtt_notify_capture(app_ah_mqtt_proto_t *p, const char *buff, size_t len)
{
    memset(&p->notify_in, 0, sizeof(p->notify_in));
    if (len > sizeof(p->notify_in))
        len = sizeof(p->notify_in);
    if (len > 0U)
        memcpy(&p->notify_in, buff, len);
}

static void _ah_mqtt_send_data(app_pcb_t *self, app_ccb_t *ccb, uint32_t ReturnLen,
                               const char *ReturnData);

static void _cmd_display(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_fill(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_restart(app_pcb_t *self, app_ccb_t *ccb, char *buff);
static void _cmd_checktime(app_pcb_t *self, app_ccb_t *ccb, char *buff);

/* 后缀与处理函数同表：索引即命令号，不会漂移。完整主题 = 前缀 + "/" + 后缀，
   由 app_ah_mqtt 在上电时拼好；探针拿完整主题 strcmp。 */
const app_ah_mqtt_cmd_entry_t g_ah_mqtt_cmd_table[AH_MQTT_CMD_COUNT] = {
    {"ASK/board/NULL", _cmd_display},
    {"ASK/display/clean", _cmd_fill},
    {"ASK/op/restart", _cmd_restart},
    {"ASK/op/checktime", _cmd_checktime},
};

static const app_font_size_t disp_size[] = {
    APP_FONT_SIZE_16,
    APP_FONT_SIZE_24,
    APP_FONT_SIZE_32,
    APP_FONT_SIZE_32,
    APP_FONT_SIZE_32,
    APP_FONT_SIZE_32,
    APP_FONT_SIZE_32,
    APP_FONT_SIZE_32,
};

/** @brief 32H 情报板显示：NotifyID + OpenClose + FontColor + FontSize + Text */
static bool _ah_mqtt_display_board(const app_ah_mqtt_cmd_display_t *para, uint32_t text_len,
                                   uint8_t color_idx, uint8_t size_idx)
{
    app_render(&(app_render_cfg_t){.type = APP_RENDER_TYPE_FILL, .color = DEV_DISPLAY_COLOR_BLACK});
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_TEXT,
        .x     = 0,
        .y     = 0,
        .w     = app_screen_rows(),
        .h     = app_screen_cols(),
        .style = &(app_render_style_t){
            .h_align   = APP_RENDER_ALIGN_CENTER,
            .v_align   = APP_RENDER_ALIGN_CENTER,
            .word_wrap = true,
        },
        .color = color_idx,
        // .text      = gbk_txt,
        // .len       = (uint16_t)gbk_len,
        .text      = para->text,
        .len       = text_len,
        .font_size = disp_size[size_idx],
        .font_type = APP_FONT_TYPE_HT,
        .text_enc  = APP_FONT_ENC_UTF8,
        .persist   = true,
    });
    return true;
}

/**
 * @brief 显示文字（按 cfg.device_id 分派设备载荷）
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void _cmd_display(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    app_ah_mqtt_proto_t *p          = _ah_mqtt_proto_of(self);
    app_ah_mqtt_cmd_display_t *para = (app_ah_mqtt_cmd_display_t *)buff;

    const dev_display_color_t text_color[] = {
        DEV_DISPLAY_COLOR_BLACK,
        DEV_DISPLAY_COLOR_GREEN,
        DEV_DISPLAY_COLOR_RED,
        DEV_DISPLAY_COLOR_YELLOW,
    };

    size_t length = strlen(buff);
    _ah_mqtt_notify_capture(p, buff, length);

    if (length < sizeof(*para)) { /* 连 OpenClose/颜色/字号都不齐：不解析、不渲染 */
        _ah_mqtt_reply_send(self, "Reply/board/NULL");
        _ah_mqtt_send_data(self, ccb, 1U, "1");
        return;
    }

    uint32_t text_len = (uint32_t)(length - sizeof(*para));

    /* 颜色/字号是 ASCII 编码，先验范围再查表：网络来的字节不能直接当下标 */
    uint8_t color_idx = (uint8_t)para->color - (uint8_t)'0';
    uint8_t size_idx  = (uint8_t)para->size - (uint8_t)'0';
    bool range_ok     = color_idx < sizeof(text_color) / sizeof(text_color[0]) &&
                        size_idx < sizeof(disp_size) / sizeof(disp_size[0]);

    bool ok = false;
    if (!range_ok) {
        printf("[ah/mqtt] 显示命令颜色/字号越界（color=0x%02X size=0x%02X）\n",
               (unsigned)(uint8_t)para->color, (unsigned)(uint8_t)para->size);
    } else {
        switch (p->cfg.device_id) {
            case AH_DEV_LED_BOARD: /* 32H：本期实现 */
                ok = _ah_mqtt_display_board(para, text_len, text_color[color_idx], size_idx);
                break;
            case AH_DEV_FEE_DISPLAY: /* 31H：载荷格式不同，后续增量 */
                printf("[ah/mqtt] 31H 费显业务载荷未实现，忽略显示命令\n");
                break;
            default:
                printf("[ah/mqtt] 未知设备类型 0x%02X，忽略显示命令\n", (unsigned)p->cfg.device_id);
                break;
        }
    }

    _ah_mqtt_reply_send(self, "Reply/board/NULL");
    _ah_mqtt_send_data(self, ccb, 1U, ok ? "0" : "1");
}

/**
 * @brief 全屏填充（兼容旧文档的清屏命令：0 黑 / 1 绿 / 2 红 / 3 黄）
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void _cmd_fill(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    app_ah_mqtt_proto_t *p       = _ah_mqtt_proto_of(self);
    app_ah_mqtt_cmd_fill_t *para = (app_ah_mqtt_cmd_fill_t *)buff;

    const dev_display_color_t fill_color[] = {
        DEV_DISPLAY_COLOR_BLACK,
        DEV_DISPLAY_COLOR_RED,
        DEV_DISPLAY_COLOR_GREEN,
        DEV_DISPLAY_COLOR_YELLOW,
    };

    size_t length = strlen(buff);
    _ah_mqtt_notify_capture(p, buff, length);

    if (length < sizeof(*para)) {
        _ah_mqtt_reply_send(self, "Reply/display/clean");
        _ah_mqtt_send_data(self, ccb, 1U, "1");
        return;
    }

    /* 同 display：颜色字节先验范围，别拿网络来的值直接当下标 */
    uint8_t color_idx = (uint8_t)para->color - (uint8_t)'0';
    if (color_idx >= sizeof(fill_color) / sizeof(fill_color[0])) {
        _ah_mqtt_reply_send(self, "Reply/display/clean");
        _ah_mqtt_send_data(self, ccb, 1U, "1");
        return;
    }

    app_render(&(app_render_cfg_t){.type = APP_RENDER_TYPE_FILL, .color = fill_color[color_idx]});

    _ah_mqtt_reply_send(self, "Reply/display/clean");
    _ah_mqtt_send_data(self, ccb, 1U, "0");
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
    app_ah_mqtt_proto_t *p = _ah_mqtt_proto_of(self);
    size_t length          = strlen(buff);
    _ah_mqtt_notify_capture(p, buff, length);

    /* 通知号都不齐的帧不能触发复位 */
    _ah_mqtt_reply_send(self, "Reply/op/restart");
    if (length < sizeof(app_ah_notify_id_t)) {
        _ah_mqtt_send_data(self, ccb, 1U, "1");
        return;
    }

    _ah_mqtt_send_data(self, ccb, 1U, "0");

    /* 给 lwIP 一点时间把应答发出去，再复位；否则平台只看到连接被掐断 */
    osDelay(100);
    NVIC_SystemReset();
}

/**
 * @brief 对时：type=1 时把 YYYYMMDDhhmmss 写入 RTC
 *
 * @param self 协议对象
 * @param ccb 通道实例
 * @param buff 命令参数
 */
static void _cmd_checktime(app_pcb_t *self, app_ccb_t *ccb, char *buff)
{
    app_ah_mqtt_proto_t *p            = _ah_mqtt_proto_of(self);
    app_ah_mqtt_cmd_checktime_t *para = (app_ah_mqtt_cmd_checktime_t *)buff;

    size_t length = strlen(buff);
    _ah_mqtt_notify_capture(p, buff, length);

    if (length < sizeof(*para)) { /* 14 字节时间串不齐：不校时 */
        _ah_mqtt_reply_send(self, "Reply/op/checktime");
        _ah_mqtt_send_data(self, ccb, 1U, "1");
        return;
    }

    bool ok = true;
    if (para->type == '1') {
        uint32_t ts = 0U;
        if (!app_ah_unix_from_datetime(para->time, sizeof(para->time), &ts) ||
            !pl_rtc_set_timestamp(pl_rtc_get_handle(), ts)) {
            printf("[ah/mqtt] 对时失败（时间串非法或 RTC 写入失败）\n");
            ok = false;
        }
    }

    _ah_mqtt_reply_send(self, "Reply/op/checktime");
    _ah_mqtt_send_data(self, ccb, 1U, ok ? "0" : "1");
}

/**
 * @brief 拼接回复主题（前缀 + 后缀）并记入协议对象，供随后的 app_ccb_send_to 使用
 *
 * @param self   协议对象（提供主题前缀与回复缓冲）
 * @param suffix 命令对应的回复后缀
 */
static void _ah_mqtt_reply_send(app_pcb_t *self, const char *suffix)
{
    app_ah_mqtt_proto_t *p = _ah_mqtt_proto_of(self);

    /* 写进协议自有缓冲，随后随消息交给通道 —— 不借用通道对象的主题缓冲区
       （那是通道的接收状态，改它等于把手伸进别人的对象） */
    int n = snprintf(p->reply_topic, sizeof(p->reply_topic), "%s/%s", p->prefix, suffix);
    if (n <= 0 || (size_t)n >= sizeof(p->reply_topic))
        p->reply_topic[0] = '\0';
}

/**
 * @brief 按协议格式发送消息（通知号头 + 数据）
 *
 * 长度显式按「20 字节通知号 + ReturnLen」给出；通知号是定长 20 字符、没有 NUL，
 * 不能用 strlen 反推（数据里出现 NUL 时会截断）。
 *
 * @param self 协议对象（提供通知号）
 * @param ccb 通道实例
 * @param ReturnLen 数据部分长度
 * @param ReturnData 数据部分
 */
static void _ah_mqtt_send_data(app_pcb_t *self, app_ccb_t *ccb, uint32_t ReturnLen,
                               const char *ReturnData)
{
    char ReturnBuff[128] = {0};

    app_ah_mqtt_proto_t *p = _ah_mqtt_proto_of(self);

    if (ccb == nullptr || ReturnData == nullptr ||
        ReturnLen > sizeof(ReturnBuff) - sizeof(app_ah_notify_id_t))
        return;

    memcpy(ReturnBuff, &p->notify_in, sizeof(app_ah_notify_id_t));
    memcpy(&ReturnBuff[sizeof(app_ah_notify_id_t)], ReturnData, ReturnLen);

    /* 目的地随消息表达：回复主题由协议给出，通道只负责翻译成自己的机制 */
    const app_ccb_dst_t dst = {.topic = p->reply_topic};
    app_ccb_send_to(ccb, &dst, (const uint8_t *)ReturnBuff,
                    (uint16_t)(sizeof(app_ah_notify_id_t) + ReturnLen));
}
