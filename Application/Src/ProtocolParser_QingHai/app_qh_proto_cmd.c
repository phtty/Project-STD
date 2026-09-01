#include "app_qh_proto_cmd.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <string.h>

#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"
#include "app_qh_proto_voice.h"
#include "text_cvt.h"

static const display_color_t s_qh_color_map[] = {
    [0] = COLOR_RED,
    [1] = COLOR_GREEN,
    [2] = COLOR_YELLOW,
};

/* 协议 1~5 → 硬件亮度；最大档映射到 DEV_DISPLAY_BRIGHTNESS_MAX(8) */
static const uint8_t s_qh_brightness_map[] = {3, 4, 5, 7, 8};
static const uint8_t s_qh_volume_map[]     = {0, 1, 2, 3, 4, 5};

/**
 * @brief  将青海协议颜色索引转换为显示颜色。
 * @param  idx  协议颜色索引。
 * @return 对应的显示颜色。
 */
static display_color_t qh_map_color(uint8_t idx)
{
    return (idx < 3) ? s_qh_color_map[idx] : COLOR_GREEN;
}

/**
 * @brief  处理主机查询命令并返回固定应答。
 * @param  ch  当前通道。
 */
static void qh_exec_host_query(channel_t *ch)
{
    static const uint8_t rsp_ok[]  = {'{', '1', 0x01, 0x00, '}'};
    static const uint8_t rsp_err[] = {'{', '1', 0x01, 0x01, '}'};
    channel_send(ch, (uint8_t *)rsp_ok, sizeof(rsp_ok));
    (void)rsp_err;
}

/**
 * @brief  执行自检命令，清屏并播报提示音。
 */
static void qh_exec_self_check(void)
{
    dev_display_t *d = dev_display_get();
    if (d) {
        dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_YELLOW);
        dev_display_commit_frame(d);
    }
    /* 「系统正在加电自检」（自检语音，UTF-8 字面量；语音板要求 GBK，运行时转换） */
    static const uint8_t text[] = "系统正在加电自检";
    uint8_t gbk[32];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK((const char *)text, (uint32_t)(sizeof(text) - 1U), (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  执行单行显示命令。
 * @param  p  单行显示参数。
 */
static void qh_exec_one_line(const qh_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;

    uint16_t w = d->screen_rows;
    if (d->screen_cols > w) w = d->screen_cols;

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = (uint16_t)p->row * FONT_16,
        .w     = w,
        .h     = FONT_16,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = true,
        },
        .color     = qh_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行全屏显示命令。
 * @param  p  全屏显示参数。
 */
static void qh_exec_full_screen(const qh_full_screen_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;

    uint16_t w = d->screen_rows;
    if (d->screen_cols > w) w = d->screen_cols;
    uint16_t h = d->screen_cols;
    if (d->screen_rows < h) h = d->screen_rows;

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = p->x,
        .y     = p->y,
        .w     = w,
        .h     = h,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = true,
        },
        .color     = qh_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  清空显示屏。
 */
static void qh_exec_clear(void)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  执行固定格式显示命令。
 * @param  p  固定格式显示参数。
 */
static void qh_exec_fixed(const qh_fixed_t *p)
{
    const uint8_t *fields[8] = {0};
    uint16_t flen[8]         = {0};
    int n                    = 0;
    const uint8_t *cur       = p->raw;
    const uint8_t *end       = p->raw + p->raw_len;
    while (cur < end && n < 8) {
        const uint8_t *sep = memchr(cur, '|', (size_t)(end - cur));
        fields[n]          = cur;
        flen[n]            = (uint16_t)((sep ? sep : end) - cur);
        n++;
        if (!sep) break;
        cur = sep + 1;
    }

    dev_display_t *d = dev_display_get();
    if (!d) return;

    uint16_t w = d->screen_rows;
    if (d->screen_cols > w) w = d->screen_cols;

    /* 固定格式显示按协议文本只做“字段拆分 + 按行显示”的基础实现。 */
    for (int i = 0; i < n && i < 5; i++) {
        if (flen[i] == 0) continue;
        if (fields[i][0] == ' ') continue;
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = (uint16_t)i * FONT_16,
            .w         = w,
            .h         = FONT_16,
            .color     = qh_map_color(p->type),
            .text      = (const char *)fields[i],
            .len       = flen[i],
            .font_size = FONT_16,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_GBK,
        });
    }

    /* 协议文本要求固定格式时同步播报收费金额；当前阶段先保留接口，
     * 如果字段未解析到金额，则不播报。 */
    if (n >= 2) {
        uint32_t amount = 0;
        for (uint16_t i = 0; i < flen[1]; i++) {
            if (fields[1][i] < '0' || fields[1][i] > '9') break;
            amount = amount * 10U + (uint32_t)(fields[1][i] - '0');
        }
        if (amount > 0U) {
            qh_voice_fee_amount(amount * 100U);
        }
    }
}

/**
 * @brief  设置屏幕亮度。
 * @param  level  亮度档位。
 */
static void qh_exec_brightness(uint8_t level)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;
    if (level == 0) {
        return; /* 自动档：交由光敏调光（上限为 DEV_DISPLAY_BRIGHTNESS_MAX） */
    }
    if (level < 1 || level > 5) return;
    dev_display_set_brightness(d, s_qh_brightness_map[level - 1]);
}

/**
 * @brief  设置语音播放音量。
 * @param  level  音量档位。
 */
static void qh_exec_volume(uint8_t level)
{
    if (level < 1 || level > 5) return;
    dev_rs232_voice_volume(s_qh_volume_map[level]);
}

/**
 * @brief  控制外设输出状态。
 * @param  ctrl  控制位图。
 */
static void qh_exec_peripheral(uint8_t ctrl)
{
    bool green  = (ctrl & 0x01U) != 0U;
    bool red    = (ctrl & 0x02U) != 0U;
    bool yellow = (ctrl & 0x04U) != 0U;
    if (red) {
        dev_io_lane_light(false);
    } else if (green) {
        dev_io_lane_light(true);
    }
    dev_io_flash_light(yellow);
}

/**
 * @brief  执行文明用语语音播报。
 * @param  idx  文明用语索引。
 */
static void qh_exec_civil_voice(uint8_t idx)
{
    qh_voice_civil(idx);
}

/**
 * @brief  执行费额语音播报。
 * @param  type        语音类型。
 * @param  amount_fen  费额，单位分。
 */
static void qh_exec_voice_fee(uint8_t type, uint32_t amount_fen)
{
    (void)type;
    qh_voice_fee_amount(amount_fen);
}

/**
 * @brief  执行青海协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 */
void qh_execute_cmd(channel_t *ch, const qh_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != QH_PARSE_OK) return;
    switch (cmd->cmd) {
        case QH_PCMD_HOST_QUERY:
            qh_exec_host_query(ch);
            break;
        case QH_PCMD_SELF_CHECK:
            qh_exec_self_check();
            break;
        case QH_PCMD_ONE_LINE:
            qh_exec_one_line(&cmd->p.one_line);
            break;
        case QH_PCMD_FULL_SCREEN:
            qh_exec_full_screen(&cmd->p.full_screen);
            break;
        case QH_PCMD_CLEAR:
            qh_exec_clear();
            break;
        case QH_PCMD_FIXED_DISPLAY:
            qh_exec_fixed(&cmd->p.fixed);
            break;
        case QH_PCMD_CIVIL_VOICE:
            qh_exec_civil_voice(cmd->p.civil.idx);
            break;
        case QH_PCMD_BRIGHTNESS:
            qh_exec_brightness(cmd->p.brightness.level);
            break;
        case QH_PCMD_VOLUME:
            qh_exec_volume(cmd->p.volume.level);
            break;
        case QH_PCMD_PERIPHERAL:
            qh_exec_peripheral(cmd->p.peripheral.ctrl);
            break;
        case QH_PCMD_VOICE:
            qh_exec_voice_fee(cmd->p.fee.type, cmd->p.fee.amount_fen);
            break;
        default:
            break;
    }
}
