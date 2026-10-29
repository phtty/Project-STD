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
#include "app_light_sensor.h"

// 命令实现和回复帧构造器前向声明
static void _cdstd_screen_switch(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_word_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_light_adjust(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_fill_screen(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_persistent_display(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_set_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_report_ip(app_ccb_t *ccb, void *data, uint16_t data_len);
static void _cdstd_picture_display(app_ccb_t *ccb, void *data, uint16_t data_len);
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
    _cdstd_picture_display,    // A7H 图片显示
    _cdstd_release_display,    // A8H 发布盒显示
};

static void _cdstd_screen_switch(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    uint8_t status = 0x31;
    uint8_t code   = 0;

    do {
        if (1 != data_len) {
            status = 0x30;
            break;
        }
        code = *(uint8_t *)data;

        if ((code != 0x30) && (code != 0x31)) {
            status = 0x30;
            break;
        }

        /* 幂等：已关再关 / 已开再开都不重复动作（每次都会置"待下发"，重复就多一次广播）。
           **不碰光敏、不缓存档位**：开关只管输出 —— 关屏期间自动跟随照常更新期望值
           （硬件仍是 0），开屏就用"当时的亮度与当时的模式"（自动还是自动、固定还是固定）。 */
        if ((!app_screen_is_oe_enabled()) && (0x31 == code))
            app_screen_oe_enable();

        if ((app_screen_is_oe_enabled()) && (0x30 == code))
            app_screen_oe_disable();

    } while (false);

    _cdstd_build_reply(ccb, 0xB0, &status, sizeof(status));
}

/* 协议码 → 本工程档位：两处的**码序都与枚举序不一致**，只能查表（协议 4.2 节）
 *   字号 0x30..0x33 = 16/24/32/48 点阵 —— app_font_size_t 的值就是像素高，算术映射不出来
 *   字体 0x30..0x33 = 黑体/仿宋/楷体/宋体 —— app_font_type_t 是 ST,FS,KT,HT，与协议序不一致
 * 两块板（本协议 5006048 独用）四档字号、四款字型都齐，无需按板裁剪。 */
static const app_font_size_t s_size_map[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_24, APP_FONT_SIZE_32, APP_FONT_SIZE_48};
static const app_font_type_t s_type_map[] = {APP_FONT_TYPE_HT, APP_FONT_TYPE_FS, APP_FONT_TYPE_KT, APP_FONT_TYPE_ST};

/** @brief 文字帧的**实际竖直范围**：单行 = 字号；显式 `\n` 每多一行加一个字号
 *
 *  不能直接把渲染区域高（= `H - y`）交给颜色账复位：`y = 0` 时区域高是整个逻辑屏，
 *  会把上下两个帧单元一起复位 —— 更新上屏会顺手把下屏颜色打回卡片色。
 *  文字是单行/显式换行、且 `word_wrap = false`（见调用点的 style），故按字号累加是精确的。 */
static uint16_t _text_frame_height(const char *text, uint16_t len, app_font_size_t size)
{
    uint16_t h = (uint16_t)size;
    for (uint16_t i = 0; i < len; i++)
        if (text[i] == '\n') h += (uint16_t)size;
    return h;
}

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
           注：**全彩口径已定**（2026-10-19）：0x30..0x37 全放行 —— 黑/红/绿/黄/蓝/紫/青/白
           （与 `app_cdstd_color_t` 一致）；0x38 起越界。 */
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
        /* 新一帧：先作废**文字实际覆盖到的**帧单元的颜色账。文字是逐字形小位图、永远盖
           不满单元，不声明新帧的话上一帧颜色会残留 —— 异色被判 mixed 后回落到卡片色
           （现场 5006048 卡片是绿，于是绿色改红/黄/蓝都停在绿）。竖直范围按字号算，
           不能用 `H - y`：`y = 0` 时那是整屏，会把下屏单元的颜色账也一起复位。 */
        app_screen_color_frame_begin(
            x, y, (uint16_t)(w - x),
            _text_frame_height((const char *)ctx->text,
                               (uint16_t)(data_len - sizeof(app_cdstd_text_display_t)),
                               s_size_map[size_i]));
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
    uint8_t status = 0x31;
    uint8_t code   = 0;

    do {
        if (1 != data_len) {
            status = 0x30;
            break;
        }
        code = *(uint8_t *)data;

        if ((code < 0x30) || (code > 0x38)) {
            status = 0x30;
            break;
        }

        if (code > 0x30)
            app_light_sensor_set_fixed(code - 0x30);
        else
            app_light_sensor_resume();

    } while (false);

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
           注：**全彩口径已定**（2026-10-19）：0x30..0x37 全放行 —— 黑/红/绿/黄/蓝/紫/青/白
           （与 `app_cdstd_color_t` 一致）；0x38 起越界。 */
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
        /* 新一帧：先作废**文字实际覆盖到的**帧单元的颜色账。文字是逐字形小位图、永远盖
           不满单元，不声明新帧的话上一帧颜色会残留 —— 异色被判 mixed 后回落到卡片色
           （现场 5006048 卡片是绿，于是绿色改红/黄/蓝都停在绿）。竖直范围按字号算，
           不能用 `H - y`：`y = 0` 时那是整屏，会把下屏单元的颜色账也一起复位。 */
        app_screen_color_frame_begin(
            x, y, (uint16_t)(w - x),
            _text_frame_height((const char *)ctx->text,
                               (uint16_t)(data_len - sizeof(app_cdstd_text_display_t)),
                               s_size_map[size_i]));
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
            .persist   = true,
            .text_enc  = APP_FONT_ENC_GBK,
            .text      = (char *)ctx->text,
            .len       = (uint16_t)(data_len - sizeof(app_cdstd_text_display_t)),
        });
    } while (false);

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

static void _cdstd_picture_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    app_cdstd_picture_display_t *ctx = data;
    uint8_t status                   = 0x31;

    do {
        if (data_len <= sizeof(app_cdstd_picture_display_t)) {
            status = 0x30;
            break;
        }

        dev_display_color_t color = (dev_display_color_t)(ctx->color - 0x30);
        if (color > DEV_DISPLAY_COLOR_WHITE) {
            status = 0x30;
            break;
        }

        /* 位置：1=上屏、2=下屏。**0（全屏）明确拒绝**：全屏位图 2800 字节，单帧装不下
           （本协议单帧上限 1403 字节 DATA）；要全屏请发两包（pos=1 + pos=2）。≥3 非法。 */
        uint16_t w = app_screen_rows();
        uint16_t h;
        uint16_t y;
        if (1 == ctx->pos) {
            h = (uint16_t)(app_screen_cols() / 2U);
            y = 0;
        } else if (2 == ctx->pos) {
            h = (uint16_t)(app_screen_cols() / 2U);
            y = (uint16_t)(app_screen_cols() / 2U);
        } else {
            status = 0x30;
            break;
        }

        /* 位图必须够铺满这一整个半边（1bpp、MSB 在前、行字节 = ceil(w/8)）：
           短一个字节都会让 app_render 按几何越界读帧缓冲 —— 与 RLS 的位图守卫同一理由。
           口径取 **≥**：多带的字节不用、也不拒。 */
        const uint16_t need = (uint16_t)(((w + 7U) / 8U) * h);
        const uint16_t have = (uint16_t)(data_len - sizeof(app_cdstd_picture_display_t));
        if (have < need) {
            status = 0x30;
            break;
        }

        /* 新一帧：整半屏位图替换该单元内容，先复位它的颜色账（换色不残留旧色） */
        app_screen_color_frame_begin(0, y, w, h);
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_BITMAP,
            .x       = 0,
            .y       = y,
            .w       = w,
            .h       = h,
            .color   = color,
            .persist = true,
            .bitmap  = ctx->bitmap,
        });

    } while (false);

    _cdstd_build_reply(ccb, 0xB7, &status, sizeof(status));
}

static void _cdstd_release_display(app_ccb_t *ccb, void *data, uint16_t data_len)
{
    (void)ccb; /* 协议 4.9 明确"无反馈"：本命令不回帧（非法帧也静默丢弃） */

    app_cdstd_release_box_t *ctx = data;

    do {
        /* 定长 DATA：3 字节头 + 1400 字节位图 = 结构体尺寸。多/少都不接（帧上限同样是 1403） */
        if (data_len != sizeof(app_cdstd_release_box_t))
            break;

        dev_display_color_t color = (dev_display_color_t)(ctx->color - 0x30);
        if (color > DEV_DISPLAY_COLOR_WHITE)
            break;

        /* 协议 4.9：DATA = 包序号(0x01–0x02) + 固化编号(0x01–0xFF) + 颜色 + 1400 字节位图。
           `pers_num` **保留不用**：本意是让下位机存多张图，但协议里没有"按编号显示已存图"的
           命令，故只解析不消费。
           seq：1 = 上半屏、2 = 下半屏。 */
        uint16_t w = app_screen_rows();
        uint16_t h;
        uint16_t y;
        if (0x01 == ctx->seq) {
            h = (uint16_t)(app_screen_cols() / 2U);
            y = 0;
        } else if (0x02 == ctx->seq) {
            h = (uint16_t)(app_screen_cols() / 2U);
            y = (uint16_t)(app_screen_cols() / 2U);
        } else {
            break;
        }

        /* 新一帧：整半屏位图替换该单元内容，先复位它的颜色账（换色不残留旧色） */
        app_screen_color_frame_begin(0, y, w, h);
        app_render(&(app_render_cfg_t){
            .type    = APP_RENDER_TYPE_BITMAP,
            .x       = 0,
            .y       = y,
            .w       = w,
            .h       = h,
            .color   = color,
            .persist = true,
            .bitmap  = ctx->bitmap,
        });
    } while (false);
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
