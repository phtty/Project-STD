/**
 * @file    app_sc_mtc_proto_cmd.c
 * @brief   四川 MTC 费显协议（1E 方案二）命令执行
 *
 * 显示映射到 app_render（GBK 直通渲染，行高 FONT_16，字型/字号可被 7B 41/42 修改）；
 * '7' 语音对接 dev_rs232_voice（参照青海 _voice.c 用法）；
 * 7B 40 改波特率通过 app_uart_baud 同步切换 RS232 + RS485 两路 UART；
 * 上行应答「收到即回」：0A 46 0A → 0A 64 0A；7B 45 → "SC_FX_P7.62_1.0"。
 */

#include "app_sc_mtc_proto_cmd.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <string.h>

#include "app_render.h"
#include "app_light_sensor.h"
#include "app_uart_baud.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"
#include "app_sc_mtc_proto_voice.h"
#include "text_cvt.h"

/* ---- 显示状态（7B 41/42/'A' 修改，后续显示命令生效）---- */
static display_color_t s_mtc_color      = COLOR_RED; /* 'A' 默认红 */
static font_size_t s_mtc_font_size      = FONT_16;   /* 7B 41 默认 16 点阵 */
static font_type_t s_mtc_font_type      = FONT_ST;   /* 7B 42 默认宋体 */

/** 7B 45 版本号应答 */
static const uint8_t s_mtc_version_text[] = "SC_FX_P7.62_1.0";

/**
 * @brief  清空整屏（置黑并提交）。
 */
static void _sc_mtc_clear_screen(void)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  渲染一行文本（当前颜色/字号/字型）。
 * @param  row    行索引 0~5（协议行号 '1'~'6'，FONT16 下 6 行屏）。
 * @param  text   文本（GBK/ASCII 混合）。
 * @param  len    文本字节数。
 * @note   先整行清黑再渲染收到内容
 *         （文本短于行宽时旧内容残留必须清除）。
 */
static void _sc_mtc_render_line(uint8_t row, const uint8_t *text, uint16_t len)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, (uint16_t)row * (uint16_t)s_mtc_font_size, d->screen_rows,
                     (uint16_t)s_mtc_font_size, COLOR_BLACK);

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = (uint16_t)row * (uint16_t)s_mtc_font_size,
        .w         = d->screen_rows,
        .h         = (uint16_t)s_mtc_font_size,
        .style     = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false,
        },
        .color     = s_mtc_color,
        .text      = (const char *)text,
        .len       = len,
        .font_size = s_mtc_font_size,
        .font_type = s_mtc_font_type,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  居中渲染单行文本（初始化提示，UTF-8 文本）。
 * @param  text  文本。
 * @param  len   文本字节数。
 */
static void _sc_mtc_render_center(const uint8_t *text, uint16_t len)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = d->screen_rows,
        .h         = d->screen_cols,
        .style     = &(render_style_t){
            .h_align   = ALIGN_CENTER,
            .v_align   = ALIGN_CENTER,
            .word_wrap = true,
        },
        .color     = s_mtc_color,
        .text      = (const char *)text,
        .len       = len,
        .font_size = FONT_SELF_ADAPT,
        .font_type = s_mtc_font_type,
        .text_enc  = FONT_ENC_UTF8,
    });
}

/**
 * @brief  执行 '1' 初始化：显示「祝您一路平安」并语音播报。
 * @note   文档要求「稍候熄灭」，本实现无定时熄灭（可后续加计时任务）。
 */
static void _sc_mtc_exec_init(void)
{
    /* 「祝您一路平安」（协议 '1' 初始化显示 + 语音，UTF-8 字面量） */
    static const uint8_t text[] = "祝您一路平安";
    _sc_mtc_render_center(text, (uint16_t)(sizeof(text) - 1U));

    /* 语音板要求 GBK：UTF-8 源文运行时转换后播报 */
    uint8_t gbk[32];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK((const char *)text, (uint32_t)(sizeof(text) - 1U), (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  执行 '2' 自检：全屏点亮 + 播报自检语音（参照青海自检实现）。
 */
static void _sc_mtc_exec_self_check(void)
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
 * @brief  执行 '3' 单行显示。
 * @param  p  单行参数（行号 + 变长文本）。
 */
static void _sc_mtc_exec_one_line(const sc_mtc_one_line_t *p)
{
    _sc_mtc_render_line(p->row, p->text, p->text_len);
}

/**
 * @brief  执行 '4' 全屏显示：先整屏清黑，变长文本按屏宽自动换行渲染。
 * @param  p  全屏参数。
 * @note   先清屏再渲染（字库引擎按屏宽自动排版）。对齐四川 ETC 全屏与
 *         治超 80 全屏先例：w=screen_rows（屏宽）、h=screen_cols（整屏高）、
 *         word_wrap=true，由渲染引擎按当前字号（7B 41 可改）自动折行。
 *         旧实现按 16B/行固定切分 ≤4 行且单行 word_wrap=false：第一行超宽时
 *         溢出被裁、总文本超 64B 被丢弃，故弃用。
 */
static void _sc_mtc_exec_full_screen(const sc_mtc_full_screen_t *p)
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
        .color     = s_mtc_color,
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = s_mtc_font_size,
        .font_type = s_mtc_font_type,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行 '6' 固定格式显示（客车 X1~X11 / 货车 X1~X20 字段布局）。
 * @param  p  固定格式参数。
 *
 * 客车（type=0，X0='0' 后 11B）：
 *   第一行「车型：X1」；第二行「金额：X2X3X4X5X6元」；第三行「余额：X7X8X9X10X11元」。
 * 货车（type=1，X0='1' 后 20B）：
 *   第一行「总重：X1X2X3.X4X5吨」；第二行「金额：X6X7X8X9X10元」；
 *   第三行「余额：X11X12X13X14X15元」；第四行「超重：X16X17X18.X19X20吨」。
 *
 * @note   文档货车示例存在两处笔误（"X1X12X13" 应为 "X1X2X3"；第四行 "总重" 应为
 *         "超重"），本实现按字段说明修正。
 */
static void _sc_mtc_exec_fixed(const sc_mtc_fixed_t *p)
{
    /* 固定格式标签（UTF-8 字面量，运行时转 GBK 参与行缓冲拼接）：
     * 车型：/ 金额：/ 余额：/ 总重：/ 超重：/ 元 / 吨 */
    uint8_t lbl_type[6], lbl_amount[6], lbl_balance[6], lbl_weight[6], lbl_over[6];
    uint8_t lbl_yuan[2], lbl_ton[2];
    uint32_t gbk_len;
    gbk_len = sizeof(lbl_type);
    UTF8ToGBK("车型：", (uint32_t)(sizeof("车型：") - 1U), (char *)lbl_type, &gbk_len);
    gbk_len = sizeof(lbl_amount);
    UTF8ToGBK("金额：", (uint32_t)(sizeof("金额：") - 1U), (char *)lbl_amount, &gbk_len);
    gbk_len = sizeof(lbl_balance);
    UTF8ToGBK("余额：", (uint32_t)(sizeof("余额：") - 1U), (char *)lbl_balance, &gbk_len);
    gbk_len = sizeof(lbl_weight);
    UTF8ToGBK("总重：", (uint32_t)(sizeof("总重：") - 1U), (char *)lbl_weight, &gbk_len);
    gbk_len = sizeof(lbl_over);
    UTF8ToGBK("超重：", (uint32_t)(sizeof("超重：") - 1U), (char *)lbl_over, &gbk_len);
    gbk_len = sizeof(lbl_yuan);
    UTF8ToGBK("元", (uint32_t)(sizeof("元") - 1U), (char *)lbl_yuan, &gbk_len);
    gbk_len = sizeof(lbl_ton);
    UTF8ToGBK("吨", (uint32_t)(sizeof("吨") - 1U), (char *)lbl_ton, &gbk_len);

    uint8_t buf[4][24]; /* 文本行缓冲（标签 6B + 数字 ≤7B + 单位 2B + '.' 1B） */
    uint16_t buf_len[4] = {0};
    uint8_t line_count  = 0;

    /* p->raw 由 parse 定位在 X1 起（X1 为首参数字节）；
     * 此前实现误再偏移一位导致各字段整体错位 */
    const uint8_t *dta = p->raw; /* X1 起 */

    if (p->type == 0U) {
        if (p->raw_len < 11U)
            return;
        /* 车型：X1 */
        memcpy(&buf[line_count][0], lbl_type, sizeof(lbl_type));
        buf[line_count][sizeof(lbl_type)] = dta[0];
        buf_len[line_count] = (uint16_t)(sizeof(lbl_type) + 1U);
        line_count++;
        /* 金额：X2~X6 元 */
        memcpy(&buf[line_count][0], lbl_amount, sizeof(lbl_amount));
        memcpy(&buf[line_count][sizeof(lbl_amount)], &dta[1], 5U);
        memcpy(&buf[line_count][sizeof(lbl_amount) + 5U], lbl_yuan, sizeof(lbl_yuan));
        buf_len[line_count] = (uint16_t)(sizeof(lbl_amount) + 5U + sizeof(lbl_yuan));
        line_count++;
        /* 余额：X7~X11 元 */
        memcpy(&buf[line_count][0], lbl_balance, sizeof(lbl_balance));
        memcpy(&buf[line_count][sizeof(lbl_balance)], &dta[6], 5U);
        memcpy(&buf[line_count][sizeof(lbl_balance) + 5U], lbl_yuan, sizeof(lbl_yuan));
        buf_len[line_count] = (uint16_t)(sizeof(lbl_balance) + 5U + sizeof(lbl_yuan));
        line_count++;
    } else {
        if (p->raw_len < 20U)
            return;
        /* 总重：X1X2X3.X4X5 吨 */
        memcpy(&buf[line_count][0], lbl_weight, sizeof(lbl_weight));
        memcpy(&buf[line_count][sizeof(lbl_weight)], &dta[0], 3U);
        buf[line_count][sizeof(lbl_weight) + 3U] = '.';
        memcpy(&buf[line_count][sizeof(lbl_weight) + 4U], &dta[3], 2U);
        memcpy(&buf[line_count][sizeof(lbl_weight) + 6U], lbl_ton, sizeof(lbl_ton));
        buf_len[line_count] = (uint16_t)(sizeof(lbl_weight) + 6U + sizeof(lbl_ton));
        line_count++;
        /* 金额：X6~X10 元 */
        memcpy(&buf[line_count][0], lbl_amount, sizeof(lbl_amount));
        memcpy(&buf[line_count][sizeof(lbl_amount)], &dta[5], 5U);
        memcpy(&buf[line_count][sizeof(lbl_amount) + 5U], lbl_yuan, sizeof(lbl_yuan));
        buf_len[line_count] = (uint16_t)(sizeof(lbl_amount) + 5U + sizeof(lbl_yuan));
        line_count++;
        /* 余额：X11~X15 元 */
        memcpy(&buf[line_count][0], lbl_balance, sizeof(lbl_balance));
        memcpy(&buf[line_count][sizeof(lbl_balance)], &dta[10], 5U);
        memcpy(&buf[line_count][sizeof(lbl_balance) + 5U], lbl_yuan, sizeof(lbl_yuan));
        buf_len[line_count] = (uint16_t)(sizeof(lbl_balance) + 5U + sizeof(lbl_yuan));
        line_count++;
        /* 超重：X16X17X18.X19X20 吨（放第 4 行，索引 3） */
        memcpy(&buf[3][0], lbl_over, sizeof(lbl_over)); /* 暂存，稍后统一渲染 */
        memcpy(&buf[3][sizeof(lbl_over)], &dta[15], 3U);
        buf[3][sizeof(lbl_over) + 3U] = '.';
        memcpy(&buf[3][sizeof(lbl_over) + 4U], &dta[18], 2U);
        memcpy(&buf[3][sizeof(lbl_over) + 6U], lbl_ton, sizeof(lbl_ton));
        uint16_t over_len = (uint16_t)(sizeof(lbl_over) + 6U + sizeof(lbl_ton));

        for (uint8_t i = 0; i < line_count; i++) {
            _sc_mtc_render_line(i, buf[i], buf_len[i]);
        }
        _sc_mtc_render_line(3, buf[3], over_len); /* 超重固定第 4 行 */
        return;
    }

    for (uint8_t i = 0; i < line_count; i++) {
        _sc_mtc_render_line(i, buf[i], buf_len[i]);
    }
}

/**
 * @brief  执行 '8' 亮度设定：0 开启自动调光（光敏），1~8 直接映射硬件档。
 * @param  val  亮度值 0~8（二进制或 ASCII 均由 parse 归一）。
 * @note   0 → 自动调光；1~8 → 关闭自动调光并直接映射硬件档。
 */
static void _sc_mtc_exec_brightness(uint8_t val)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    if (val == 0U) {
        if (g_light_sensor_task_handle != nullptr)
            osThreadResume(g_light_sensor_task_handle); /* 开启自动调光（恢复光敏任务） */
        return;
    }
    if (g_light_sensor_task_handle != nullptr)
        osThreadSuspend(g_light_sensor_task_handle); /* 关闭自动调光 */
    dev_display_set_brightness(d, val);
}

/**
 * @brief  执行 'A' 显示颜色设定：'1'红 / '2'黄 / '3'绿。
 * @param  val  颜色索引 1~3。
 */
static void _sc_mtc_exec_color(uint8_t val)
{
    static const display_color_t color_map[] = {
        [1] = COLOR_RED,
        [2] = COLOR_YELLOW,
        [3] = COLOR_GREEN,
    };
    s_mtc_color = color_map[val];
}

/**
 * @brief  执行 7B 40 修改波特率：按文档波特率低 16 位切换 RS232 + RS485。
 * @param  p  波特率参数。
 * @note   0x2580=9600、0xC200=115200（低 16 位）；切换后本通道以新波特率继续工作，
 *         主机亦须同步切换。文档未定义应答帧，不回。
 */
static void _sc_mtc_exec_baud(const sc_mtc_baud_t *p)
{
    uint32_t baud = (p->baud16 == 0x2580U) ? 9600U : 115200U;
    (void)app_uart_baud_apply(baud);
}

/**
 * @brief  执行 7B 44 全屏点亮：00 红 / 01 绿 / 02 黄 全屏填充。
 * @param  val  颜色索引。
 */
static void _sc_mtc_exec_fill_all(uint8_t val)
{
    static const display_color_t color_map[] = {
        [0] = COLOR_RED,
        [1] = COLOR_GREEN,
        [2] = COLOR_YELLOW,
    };
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, color_map[val]);
    dev_display_commit_frame(d);
}

/**
 * @brief  执行 MTC 协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 * @note   上行应答「收到即回」：主机查询 0A 46 0A → 0A 64 0A；
 *         7B 45 取版本号 → "SC_FX_P7.62_1.0"。其余命令文档未定义应答，不回。
 */
void sc_mtc_execute_cmd(channel_t *ch, const sc_mtc_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != SC_MTC_PARSE_OK)
        return;

    switch (cmd->cmd) {
        case SC_MTC_PCMD_INIT:
            _sc_mtc_exec_init();
            break;
        case SC_MTC_PCMD_SELF_CHECK:
            _sc_mtc_exec_self_check();
            break;
        case SC_MTC_PCMD_ONE_LINE:
            _sc_mtc_exec_one_line(&cmd->p.one_line);
            break;
        case SC_MTC_PCMD_FULL_SCREEN:
            _sc_mtc_exec_full_screen(&cmd->p.full_screen);
            break;
        case SC_MTC_PCMD_CLEAR:
            _sc_mtc_clear_screen();
            break;
        case SC_MTC_PCMD_FIXED_DISPLAY:
            _sc_mtc_exec_fixed(&cmd->p.fixed);
            break;
        case SC_MTC_PCMD_VOICE:
            if (cmd->p.voice.idx == 8U) {
                dev_rs232_voice_play(cmd->p.voice.text, cmd->p.voice.text_len);
            } else {
                sc_mtc_voice_play_fixed(cmd->p.voice.idx);
            }
            break;
        case SC_MTC_PCMD_BRIGHTNESS:
            _sc_mtc_exec_brightness(cmd->p.byte_val.val);
            break;
        case SC_MTC_PCMD_VOLUME:
            dev_rs232_voice_volume(cmd->p.byte_val.val);
            break;
        case SC_MTC_PCMD_COLOR:
            _sc_mtc_exec_color(cmd->p.byte_val.val);
            break;
        case SC_MTC_PCMD_RAW_BAUD:
            _sc_mtc_exec_baud(&cmd->p.baud);
            break;
        case SC_MTC_PCMD_RAW_DOT_SIZE: {
            static const font_size_t dot_map[] = {FONT_16, FONT_24, FONT_32};
            s_mtc_font_size = dot_map[cmd->p.byte_val.val];
            break;
        }
        case SC_MTC_PCMD_RAW_FONT: {
            static const font_type_t font_map[] = {FONT_ST, FONT_FS, FONT_KT, FONT_HT};
            s_mtc_font_type = font_map[cmd->p.byte_val.val];
            break;
        }
        case SC_MTC_PCMD_RAW_PROTO:
            /* 设置协议类型（治超屏/ETC/治超屏）：本固件已同编三协议，无需切换，仅记录 */
            break;
        case SC_MTC_PCMD_RAW_FILL_ALL:
            _sc_mtc_exec_fill_all(cmd->p.byte_val.val);
            break;
        case SC_MTC_PCMD_RAW_VERSION:
            channel_send(ch, (uint8_t *)s_mtc_version_text, sizeof(s_mtc_version_text) - 1U);
            break;
        case SC_MTC_PCMD_HOST_QUERY: {
            static const uint8_t rsp_ok[3] = {0x0A, 0x64, 0x0A};
            channel_send(ch, (uint8_t *)rsp_ok, sizeof(rsp_ok));
            break;
        }
        case SC_MTC_PCMD_HOST_CLEAR:
            _sc_mtc_clear_screen();
            break;
        default:
            break;
    }
}