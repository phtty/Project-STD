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

static void _cdstd_screen_switch(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_pictrue_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_release_display(app_ccb_t *ccb, void *data, uint16_t data_len);

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
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_pictrue_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}

static void _cdstd_release_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb;
    (void)data;
    (void)data_len;
}