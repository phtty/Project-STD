/**
 * @file    app_cdstd_cmd.c
 * @brief   CDSTD 命令处理实现: 逐命令解析请求帧并拼装应答
 */

#include "app_cdstd_cmd.h"

#include "crc_utils.h"

#include "pl_net.h"

#include "app_udp.h"
#include "app_rs485.h"
#include "app_render.h"

// 命令实现和回复帧构造器前向声明
static void _cdstd_screen_switch(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_pictrue_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_release_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_build_reply(app_ccb_t *ccb, uint8_t cmd, const uint8_t *data, uint16_t len);

const app_cdstd_cmd_handler_fn_t g_cdstd_cmd_table[] = {
    _cdstd_screen_switch,      // A0H 屏开关
    _cdstd_word_display,       // A1H 文字显示
    _cdstd_light_adjust,       // A2H 亮度调节
    _cdstd_fill_screen,        // A3H 全屏填充
    _cdstd_persistent_display, // A4H 半屏持久化显示
    _cdstd_set_ip,             // A5H 设置ip地址
    _cdstd_report_ip,          // A6H 上报ip地址
    _cdstd_pictrue_display,    // A7H 图片
    _cdstd_release_display,    // A8H 发布盒显示
};

static void _cdstd_screen_switch(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB0, &status, 1);
}

static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB1, &status, 1);
}

static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB2, &status, 1);
}

static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB3, &status, 1);
}

static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB4, &status, 1);
}

static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB5, &status, 1);
}

static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
    // uint8_t net_cfg[14] = {0};

    // _cdstd_build_reply(ccb, 0xB6, , 14);
}

static void _cdstd_pictrue_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB7, &status, 1);
}

static void _cdstd_release_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

/**
 * @brief 构造协议返回帧
 *
 * @param ccb 通道控制块
 * @param cmd 命令码
 * @param data 返回的数据内容
 * @param len
 */
static void _cdstd_build_reply(app_ccb_t *ccb, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    static uint8_t _buf[sizeof(app_cdstd_reply_ip_t)] = {0};
    memset(_buf, 0, sizeof(_buf));

    if (0xB6 == cmd) {
        app_cdstd_reply_ip_t *frame = (app_cdstd_reply_ip_t *)_buf;
        if (len < sizeof(frame->ip) + sizeof(frame->mask) + sizeof(frame->gw) + sizeof(frame->port))
            return;

        frame->head   = 0xFFFF;
        frame->len[1] = 0x0F;
        frame->cmd    = cmd;
        memcpy(frame->ip, data, sizeof(frame->ip));
        memcpy(frame->mask, data + sizeof(frame->ip), sizeof(frame->mask));
        memcpy(frame->gw, data + sizeof(frame->ip) + sizeof(frame->mask), sizeof(frame->gw));
        memcpy(frame->port, data + sizeof(frame->ip) + sizeof(frame->gw) + sizeof(frame->mask), sizeof(frame->port));
        uint16_t crc = crc16_xmodem(_buf + sizeof(frame->head), sizeof(*frame) - sizeof(frame->head) - sizeof(frame->crc));
        frame->crc[0] |= (uint8_t)(crc >> 8);
        frame->crc[1] |= (uint8_t)crc;

        app_ccb_dst_t dst = {.broadcast = true};
        app_ccb_send_to(ccb, &dst, _buf, sizeof(app_cdstd_reply_ip_t));

    } else {
        app_cdstd_reply_status_t *frame = (app_cdstd_reply_status_t *)_buf;
        if (len != 1)
            return;

        frame->head   = 0xFFFF;
        frame->len[1] = 0x02;
        frame->cmd    = cmd;
        frame->status = *data;
        uint16_t crc  = crc16_xmodem(_buf + sizeof(frame->head), sizeof(*frame) - sizeof(frame->head) - sizeof(frame->crc));
        frame->crc[0] |= (uint8_t)(crc >> 8);
        frame->crc[1] |= (uint8_t)crc;
        app_ccb_send(ccb, _buf, sizeof(app_cdstd_reply_status_t));
    }
}
