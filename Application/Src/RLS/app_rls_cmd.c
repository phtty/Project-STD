#include "app_rls_cmd.h"

#include "app_render.h"
#include "app_light_sensor.h"
#include "app_iap_cfg.h"
#include "app_udp.h"
#include "app_rls_pic.h"
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

/* 顺序必须与 app_rls.c 的 cmd_index_table 一致（REPORT_IP 仅设备→主机方向，不在接收分派中） */
const rls_cmd_handler_fn_t g_rls_cmd_table[] = {
    cmd_test,
    cmd_display_sw,
    cmd_display_tmp,
    cmd_display_save,
    cmd_display_pic,
    cmd_inquiry,
    cmd_set_ip,
    cmd_aquire_ip,
};

[[maybe_unused]] static void cmd_test(channel_t *ch, void *data)
{
    (void)ch;
    (void)data;
}

/* DISPLAY_SW/TMP/SAVE/PIC 四个显示命令共用：应用 rls_dispaly_t.light_level 字段
 * 0=关屏, 1-7=开屏+固定亮度（停用自动调光）, 255=开屏+恢复自动调光, 其他值忽略 */
static void rls_apply_light_level(uint8_t light_level)
{
    switch (light_level) {
        case 0: /* 关屏：必须同时停用自动调光，否则光感任务 1s 内把亮度写回 1~7 */
            app_light_sensor_set_auto(false);
            dev_display_set_brightness(dev_display_get(), 0);
            break;
        case 1 ... 7: /* 开屏 + 固定亮度（停用自动调光） */
            app_light_sensor_set_auto(false);
            dev_display_set_brightness(dev_display_get(), light_level);
            break;
        case 255: /* 开屏 + 恢复自动调光（内部立即调光一次） */
            app_light_sensor_set_auto(true);
            break;
        default: /* 大于7小于255则使用等级7亮度 */
            app_light_sensor_set_auto(false);
            dev_display_set_brightness(dev_display_get(), 7);
            break;
    }
}

static void cmd_display_sw(channel_t *ch, void *data)
{
    (void)ch;
    rls_apply_light_level(((rls_dispaly_t *)data)->light_level);
}

static void cmd_display_tmp(channel_t *ch, void *data)
{
    (void)ch;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;
    rls_apply_light_level(display_ctx->light_level); /* 显示命令同步应用亮度 */

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

    if (display_ctx->pic_num >= RLS_PIC_SLOT_COUNT)
        return; /* 越界：整条命令忽略，无任何副作用 */

    rls_apply_light_level(display_ctx->light_level); /* 显示命令同步应用亮度 */
    rls_pic_save(display_ctx->pic_num, display_ctx->bitmap, display_ctx->color); /* 写缓存槽 + 持久化 */
    rls_pic_show(display_ctx->pic_num);                                           /* 显示（内部同步渲染持久化） */
}

static void cmd_display_pic(channel_t *ch, void *data)
{
    (void)ch;
    rls_dispaly_t *display_ctx = (rls_dispaly_t *)data;

    if (display_ctx->pic_num >= RLS_PIC_SLOT_COUNT)
        return; /* 越界：忽略（畸形帧语义，不清屏） */

    rls_apply_light_level(display_ctx->light_level); /* 显示命令同步应用亮度 */
    rls_pic_show(display_ctx->pic_num);              /* 槽无效时内部清屏 + 同步渲染持久化 */
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

    frame->head[0]                = 0xFF;
    frame->head[1]                = 0xFE;
    frame->length[0]              = 0x00;
    frame->length[1]              = 21;                /* 帧全长 = 6 + 12 + BCC 1 + 尾 2，大端 */
    *(rls_cmd_type_t *)frame->cmd = RLS_CMD_REPORT_IP; /* 上报方向命令码，线上小端（与探测端一致） */

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
