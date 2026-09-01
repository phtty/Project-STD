/**
 * @file    app_sc_ol_proto_cmd.c
 * @brief   四川治超屏协议（1F，3.5.1 串口方式）命令执行
 *
 * 显示映射到 app_render（GBK 直通渲染，行高 FONT_16）；
 * 通行灯/黄闪映射到 dev_io_lane_light / dev_io_flash_light；
 * 查询类应答「收到即回」，回显本模块维护的当前状态。
 *
 * 应答帧构造说明（文档 3.5.1 (7) 示例内部不一致）：
 *   文档 A1~A5 示例长度字段为 0x20(32) 但示例帧实际 20 字节（数据 14B，7 字），
 *   空行示例仅 7 字节。本实现维持「16B/行」固定应答（FF 16 A<n> <亮度> <16B 数据> BCC FF）
 *   以兼容既有测试工具；行存储本身已变长（≤24B，不足不补空格、超长截断）。
 *   B6/B9/B8 应答按文档原文回显：FF 07 <命令> 00 <当前值> BCC FF。
 */

#include "app_sc_ol_proto_cmd.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <string.h>

#include "app_render.h"
#include "app_light_sensor.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"

/* ---- 当前屏状态（查询应答回显用）---- */
static uint8_t s_ol_lines[SC_OL_LINE_COUNT][SC_OL_LINE_TEXT_MAX]; /**< 八行显示内容（空行 0x00） */
static uint8_t s_ol_line_len[SC_OL_LINE_COUNT]; /**< 各行实际文本长度（变长数据） */
static uint8_t s_ol_brightness = 0xFF; /**< 当前亮度（00 最暗，FF 最亮，初始最亮） */
static uint8_t s_ol_lane       = 0x00; /**< 通行灯状态（00 红 / 01 绿） */
static uint8_t s_ol_flash      = 0x00; /**< 黄闪状态（00 关 / 01 开） */

/** 显示颜色跟踪：显示颜色跟随通行灯状态
 *  （99 帧 00=红 / 01=绿；上电默认绿）。 */
static display_color_t s_ol_color = COLOR_GREEN;

/**
 * @brief  治超帧 BCC 计算：帧头(含)到数据段(含)逐字节异或（不含尾部 FF）。
 * @param  raw      帧缓冲。
 * @param  raw_len  帧长度。
 * @return BCC 值。
 */
static uint8_t _sc_ol_bcc_calc(const uint8_t *raw, uint16_t raw_len)
{
    uint8_t bcc = 0;
    for (uint16_t i = 0; i + 2U < raw_len; i++) { /* [0, len-3]，不含 BCC(len-2) 与尾 FF(len-1) */
        bcc ^= raw[i];
    }
    return bcc;
}

/**
 * @brief  构造治超应答帧并发送。
 * @param  ch        当前通道。
 * @param  cmd       应答命令字节（A1~A4 / B6 / B9 / B8）。
 * @param  bright    亮度字段。
 * @param  data      数据段（可为 NULL）。
 * @param  data_len  数据段字节数。
 */
static void _sc_ol_send_reply(channel_t *ch, uint8_t cmd, uint8_t bright,
                              const uint8_t *data, uint16_t data_len)
{
    uint8_t buf[SC_OL_FRAME_LEN_MAX];
    uint16_t len = (uint16_t)(6U + data_len); /* FF+len+cmd+bright+data+BCC+FF */
    buf[0]       = 0xFF;
    buf[1]       = (uint8_t)len;
    buf[2]       = cmd;
    buf[3]       = bright;
    if (data_len > 0U && data != NULL) {
        memcpy(&buf[4], data, data_len);
    }
    buf[len - 2U] = _sc_ol_bcc_calc(buf, len);
    buf[len - 1U] = 0xFF;
    channel_send(ch, buf, len);
}

/**
 * @brief  清空整屏（置黑并提交），并复位行状态。
 */
static void _sc_ol_clear_screen(void)
{
    memset(s_ol_lines, 0, sizeof(s_ol_lines));
    memset(s_ol_line_len, 0, sizeof(s_ol_line_len));
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  渲染一行并更新行状态。
 * @param  p  行显示参数。
 */
static void _sc_ol_exec_line(const sc_ol_line_t *p)
{
    /* 变长数据存储（不足不补空格，>24B 截断） */
    uint8_t n = (uint8_t)((p->text_len > SC_OL_LINE_TEXT_MAX) ? SC_OL_LINE_TEXT_MAX : p->text_len);
    memset(s_ol_lines[p->row], 0, SC_OL_LINE_TEXT_MAX);
    memcpy(s_ol_lines[p->row], p->text, n);
    s_ol_line_len[p->row] = n;

    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    /* 先整行清黑再渲染收到内容 */
    dev_display_fill(d, 0, (uint16_t)p->row * FONT_16, d->screen_rows, FONT_16, COLOR_BLACK);
    dev_display_commit_frame(d);

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = (uint16_t)p->row * FONT_16,
        .w         = d->screen_rows,
        .h         = FONT_16,
        .style     = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false,
        },
        .color     = s_ol_color, /* 显示颜色跟随通行灯状态 */
        .text      = (const char *)s_ol_lines[p->row],
        .len       = n,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行 80 全屏显示：先整屏清黑再自动换行渲染（0 数据帧 = 清屏）。
 * @note   先整屏清黑再按屏宽自动排版渲染。
 * @param  p  全屏显示参数。
 */
static void _sc_ol_exec_full_screen(const sc_ol_full_screen_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    if (p->text_len == 0U) {
        dev_display_commit_frame(d);
        return;
    }

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = d->screen_rows,
        .h         = d->screen_cols,
        .style     = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = true,
        },
        .color     = s_ol_color,
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行亮度调节：XX 00=自动调光 / 01~FF 映射硬件 1~8 档（FF 最亮）。
 * @note   val==0 → 开启自动调光；非 0 → 关闭自动调光，硬件档=(val+1)/32。
 * @param  val  亮度原始值。
 */
static void _sc_ol_exec_brightness(uint8_t val)
{
    s_ol_brightness = val;
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    if (val == 0U) {
        if (g_light_sensor_task_handle != nullptr)
            osThreadResume(g_light_sensor_task_handle); /* 开启自动调光（恢复光敏任务） */
        return;
    }
    if (g_light_sensor_task_handle != nullptr)
        osThreadSuspend(g_light_sensor_task_handle);    /* 关闭自动调光 */
    uint8_t level = (uint8_t)((val + 1U) / 32U);
    /* 0 档根因防护：val=1~31 时 (val+1)/32=0，light_level=0 → OE 恒高 → 整屏全灭。
     * 硬件档位必须落在 1~8，最低钳制为 1。 */
    if (level == 0U)
        level = 1U;
    dev_display_set_brightness(d, level); /* (val+1)/32：31~62→1 … 224~255→8 */
}

/**
 * @brief  执行 A0 查询：逐行回 A1~A8 独立帧。
 * @param  ch  当前通道。
 */
static void _sc_ol_exec_query_content(channel_t *ch)
{
    for (uint8_t i = 0; i < SC_OL_LINE_COUNT; i++) {
        _sc_ol_send_reply(ch, (uint8_t)(0xA1U + i), s_ol_brightness,
                          s_ol_lines[i], SC_OL_BYTES_PER_LINE);
    }
}

/**
 * @brief  执行治超协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void sc_ol_execute_cmd(channel_t *ch, const sc_ol_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != SC_OL_PARSE_OK)
        return;

    switch (cmd->cmd) {
        case SC_OL_PCMD_FULL_SCREEN:
            _sc_ol_exec_full_screen(&cmd->p.full_screen);
            break;
        case SC_OL_PCMD_LINE_1:
        case SC_OL_PCMD_LINE_2:
        case SC_OL_PCMD_LINE_3:
        case SC_OL_PCMD_LINE_4:
        case SC_OL_PCMD_LINE_5:
        case SC_OL_PCMD_LINE_6:
        case SC_OL_PCMD_LINE_7:
        case SC_OL_PCMD_LINE_8:
            _sc_ol_exec_line(&cmd->p.line);
            break;
        case SC_OL_PCMD_CLEAR:
            _sc_ol_clear_screen();
            break;
        case SC_OL_PCMD_BRIGHTNESS:
            _sc_ol_exec_brightness(cmd->p.byte_val.val);
            break;
        case SC_OL_PCMD_LANE_LIGHT:
            s_ol_lane = cmd->p.byte_val.val;
            /* 通行灯命令同步更新显示颜色 */
            s_ol_color = (s_ol_lane != 0U) ? COLOR_GREEN : COLOR_RED;
            dev_io_lane_light(s_ol_lane != 0U); /* 00 红 / 01 绿 */
            break;
        case SC_OL_PCMD_YELLOW_FLASH:
            s_ol_flash = cmd->p.byte_val.val;
            dev_io_flash_light(s_ol_flash != 0U); /* 00 关 / 01 开 */
            break;
        case SC_OL_PCMD_QUERY_CONTENT:
            _sc_ol_exec_query_content(ch);
            break;
        case SC_OL_PCMD_QUERY_BRIGHT:
            _sc_ol_send_reply(ch, 0xB6, 0x00, &s_ol_brightness, 1U);
            break;
        case SC_OL_PCMD_QUERY_LANE:
            _sc_ol_send_reply(ch, 0xB9, 0x00, &s_ol_lane, 1U);
            break;
        case SC_OL_PCMD_QUERY_FLASH:
            _sc_ol_send_reply(ch, 0xB8, 0x00, &s_ol_flash, 1U);
            break;
        default:
            break;
    }
}