#include "app_rls_cmd.h"

#include "app_render.h"
#include "app_light_sensor.h"
#include "app_iap_cfg.h"
#include "app_udp.h"
#include "pl_net.h"
#include "bcc_utils.h"

static void cmd_test(channel_t *ch, void *data);
static void cmd_display_sw(channel_t *ch, void *data);
static void cmd_display_tmp(channel_t *ch, void *data);
static void cmd_display_save(channel_t *ch, void *data);
static void cmd_display_pic(channel_t *ch, void *data);
static void cmd_inquiry(channel_t *ch, void *data);
static void cmd_set_ip(channel_t *ch, void *data);
static void cmd_aquire_ip(channel_t *ch, void *data);
static void cmd_report_ip(channel_t *ch, void *data);

/* 顺序必须与 app_rls.c 的 cmd_index_table 一致 */
const rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    cmd_test,
    cmd_display_sw,
    cmd_display_tmp,
    cmd_display_save,
    cmd_display_pic,
    cmd_inquiry,
    cmd_set_ip,
    cmd_aquire_ip,
    cmd_report_ip,
};

/* ======================================================================
 * 内置 16×16 位图（每行 2 字节，MSB-first），当前为占位图案！
 * TODO: 待用户提供真实信号灯位图数据后替换
 * ====================================================================== */
static const uint8_t s_rls_pic_0[32] = {
    0xFF, 0xFF, /* 空心方框 */
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01,
    0xFF, 0xFF,
};
static const uint8_t s_rls_pic_1[32] = {
    0x80, 0x01, /* 对角交叉 X */
    0xC0, 0x03, 0x60, 0x06, 0x30, 0x0C,
    0x18, 0x18, 0x0C, 0x30, 0x06, 0x60, 0x03, 0xC0,
    0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x18, 0x18,
    0x30, 0x0C, 0x60, 0x06, 0xC0, 0x03, 0x80, 0x01,
};

[[maybe_unused]] static void cmd_test(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
}

static void cmd_display_sw(channel_t *ch, void *data)
{
    (void)ch;
    uint8_t sw = *(uint8_t *)data;

    switch (sw) {
        case 0: /* 关屏：必须同时停用自动调光，否则光感任务 1s 内把亮度写回 1~7 */
            app_light_sensor_set_auto(false);
            dev_display_set_brightness(dev_display_get(), 0);
            break;
        case 1 ... 7: /* 开屏 + 固定亮度（停用自动调光） */
            app_light_sensor_set_auto(false);
            dev_display_set_brightness(dev_display_get(), sw);
            break;
        case 255: /* 开屏 + 恢复自动调光（内部立即调光一次） */
            app_light_sensor_set_auto(true);
            break;
        default: /* 其他值忽略 */
            break;
    }
}

static void cmd_display_tmp(channel_t *ch, void *data)
{
    (void)ch;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
}

static void cmd_display_save(channel_t *ch, void *data)
{
    (void)ch;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = display_ctx->color,
        .bitmap = display_ctx->bitmap,
    });
    app_render_save();
}

static void cmd_display_pic(channel_t *ch, void *data)
{
    (void)ch;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;
    uint8_t pic_num            = (display_ctx->pic_num > 1) ? 0 : display_ctx->pic_num; /* 仅内置 2 张，越界回退 0 */

    // 显示前先清屏
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render(&(render_cfg_t){
        .type   = RENDER_BITMAP,
        .x      = 0,
        .y      = 0,
        .w      = dev_display_get()->screen_rows,
        .h      = dev_display_get()->screen_cols,
        .color  = (display_color_t)display_ctx->color, /* 颜色取帧内 color 字段 */
        .bitmap = pic_num ? s_rls_pic_1 : s_rls_pic_0,
    });
}

static void cmd_inquiry(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
    /* 功能保留，不做响应 */
}

static void cmd_set_ip(channel_t *ch, void *data)
{
    (void)ch;
    /* data = ip[4] | mask[4] | gw[4]，仅写 IAP Flash，重启后生效 */
    uint8_t *p = (uint8_t *)data;
    app_flash_iap_update_net_cfg(p, p + 4, p + 8);
}

/* REPORT_IP 响应帧缓冲：定长 21B。RLS 仅单处理任务，无需互斥锁 */
static uint8_t s_rls_tx_buf[32];

static void cmd_aquire_ip(channel_t *ch, void *data)
{
    (void)data;
    rls_frame_t *frame = (rls_frame_t *)s_rls_tx_buf;
    uint8_t *payload   = frame->data_bcc_tail;
    uint8_t ip[4], mask[4], gw[4];

    frame->head[0]   = 0xFF;
    frame->head[1]   = 0xFE;
    frame->length[0] = 0x00;
    frame->length[1] = 21; /* 帧全长 = 6 + 12 + BCC 1 + 尾 2，大端 */
    frame->cmd[0]    = 0x42; /* RLS_CMD_REPORT_IP = 0x4252，线上小端（与探测端一致） */
    frame->cmd[1]    = 0x52;

    pl_net_get_ip(ip, mask, gw); /* 取当前实际生效的地址 */
    memcpy(payload, ip, 4);
    memcpy(payload + 4, mask, 4);
    memcpy(payload + 8, gw, 4);
    payload[12] = bcc_calcu(payload, 12); /* BCC 对 data 域 XOR */
    payload[13] = 0x0D;
    payload[14] = 0x0C;

    app_udp_broadcast(s_rls_tx_buf, 21); /* 向上位机广播本机 IP（255.255.255.255:10011） */
    if (ch->ch_id == CH_ID_RS485 || ch->ch_id == CH_ID_RS232 || ch->ch_id == CH_ID_RS232_1)
        channel_send(ch, s_rls_tx_buf, 21); /* UART 来源：原路再回一帧 */
}

static void cmd_report_ip(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
    /* 设备收到自己广播回环的 REPORT_IP 或他机上报时静默忽略，防死循环 */
}
