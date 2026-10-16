/**
 * @file    app_cdstd_cmd.c
 * @brief   CDSTD 命令处理实现: 逐命令解析请求帧并拼装应答
 */

#include "app_cdstd_cmd.h"

#include "crc_utils.h"

#include "pl_net.h"

#include "app_cdstd_cfg.h"
#include "app_iap_cfg.h"
#include "app_udp.h"
#include "app_rs485.h"
#include "app_screen.h"
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

    _cdstd_build_reply(ccb, 0xB0, &status, sizeof(status));
}

/* 协议码 → 本工程档位：两处的**码序都与枚举序不一致**，只能查表（协议 4.2 节）
 *   字号 0x30..0x33 = 16/24/32/48 点阵 —— app_font_size_t 的值就是像素高，算术映射不出来
 *   字体 0x30..0x33 = 黑体/仿宋/楷体/宋体 —— app_font_type_t 是 ST,FS,KT,HT，与协议序不一致
 * 两块板（本协议 5006048 独用）四档字号、四款字型都齐，无需按板裁剪。 */
static const app_font_size_t s_size_map[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_24, APP_FONT_SIZE_32, APP_FONT_SIZE_48};
static const app_font_type_t s_type_map[] = {APP_FONT_TYPE_HT, APP_FONT_TYPE_FS, APP_FONT_TYPE_KT, APP_FONT_TYPE_ST};

static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    app_cdstd_text_display_t *ctx = data;
    uint8_t status                = 0x31;

    do {
        if (data_len <= sizeof(app_cdstd_text_display_t)) { /* 文字数据至少 1 字节；清屏走 A3 */
            status = 0x30;
            break;
        }

        /* 协议码 0x30..0x33 → 表下标。低于 0x30 的码经 uint8_t 回绕成大数，同一句即可拦下 */
        uint8_t size_i = (uint8_t)(ctx->font_size - 0x30);
        if (size_i >= sizeof(s_size_map) / sizeof(s_size_map[0])) {
            status = 0x30;
            break;
        }
        uint8_t type_i = (uint8_t)(ctx->font_type - 0x30);
        if (type_i >= sizeof(s_type_map) / sizeof(s_type_map[0])) {
            status = 0x30;
            break;
        }

        /* 颜色同 A3：先转换再判据。直接判 `ctx->color - 0x30 > WHITE` 会漏放低位
           （负的 int 不满足 >），而回绕后的值一定 > WHITE。
           注：0x34..0x37（蓝/紫/青/白）是否放行的口径待与协议方确认，此处与 A3 保持一致。 */
        dev_display_color_t color = (dev_display_color_t)(ctx->color - 0x30);
        if (color > DEV_DISPLAY_COLOR_WHITE) {
            status = 0x30;
            break;
        }

        uint16_t x = (uint16_t)((ctx->x[0] << 8) | ctx->x[1]);
        uint16_t y = (uint16_t)((ctx->y[0] << 8) | ctx->y[1]);
        uint16_t w = app_screen_rows(), h = app_screen_cols();
        if ((x >= w) || (y >= h)) {
            status = 0x30;
            break;
        }

        /* x/y = 文字左上角（协议只写"坐标"）：区域取剩余 [x,W)×[y,H)，对齐放左上角，
           否则"水平居中 + 满屏宽"会让 x 形同虚设 */
        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_TEXT,
            .x     = x,
            .y     = y,
            .w     = (uint16_t)(w - x),
            .h     = (uint16_t)(h - y),
            .style = &(app_render_style_t){
                .h_align = APP_RENDER_ALIGN_LEFT_UP,
                .v_align = APP_RENDER_ALIGN_LEFT_UP,
            },
            .color     = color,
            .font_size = s_size_map[size_i],
            .font_type = s_type_map[type_i],
            .text_enc  = APP_FONT_ENC_GBK,
            .text      = (char *)ctx->text,
            .len       = (uint16_t)(data_len - sizeof(app_cdstd_text_display_t)),
        });
    } while (false);

    _cdstd_build_reply(ccb, 0xB1, &status, sizeof(status));
}

static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB2, &status, sizeof(status));
}

static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    dev_display_color_t color = *(uint8_t *)data - 0x30;
    uint8_t status            = 0x31;

    do {
        if (color > DEV_DISPLAY_COLOR_WHITE) {
            status = 0x30;
            break;
        }

        if (1 != data_len) {
            status = 0x30;
            break;
        }

        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_FILL,
            .x     = 0,
            .y     = 0,
            .w     = app_screen_rows(),
            .h     = app_screen_cols(),
            .color = color,
        });
    } while (false);

    _cdstd_build_reply(ccb, 0xB3, &status, sizeof(status));
}

static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB4, &status, sizeof(status));
}

static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    app_cdstd_net_cfg_t *info = data;
    uint8_t status            = 0x31;

    do {
        if (data_len != sizeof(app_cdstd_net_cfg_t)) {
            status = 0x30;
            break;
        }

        if (0 != app_cdstd_save_config(info))
            status = 0x30;

        uint16_t port = (uint16_t)(info->port[0] << 8) | (uint8_t)(info->port[1]);
        app_flash_iap_update_net_cfg(info->ip, info->mask, info->gw, port);
    } while (false);

    _cdstd_build_reply(ccb, 0xB5, &status, sizeof(status));
}

static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    app_cdstd_net_cfg_t info = {0};

    if (!app_cdstd_load_config(&info)) {
        pl_net_get_ip(info.ip, info.mask, info.gw);
        info.port[0] = (uint8_t)(app_udp_get_port() >> 8);
        info.port[1] = (uint8_t)(app_udp_get_port());
    }

    _cdstd_build_reply(ccb, 0xB6, (uint8_t *)&info, sizeof(info));
}

static void _cdstd_pictrue_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)data;
    (void)data_len;
    uint8_t status = 0x31;

    _cdstd_build_reply(ccb, 0xB7, &status, sizeof(status));
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
