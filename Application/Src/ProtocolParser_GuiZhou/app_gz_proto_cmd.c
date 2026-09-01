/**
 * @file    app_gz_proto_cmd.c
 * @brief   贵州常规费显协议命令执行
 *
 * 显示内容映射到 app_render（协议文本 GBK 直通渲染，构造行文 UTF-8 渲染，行高 FONT_16）；
 * 灯控映射到 dev_io_lane_light / dev_io_flash_light（红优先）；
 * 亮度 0=自动调光（恢复光敏任务），1~5 映射硬件档 {3,4,5,7,8}；
 * 音量 1~5 映射语音板音量 {1,3,5,7,9}（按设备实测行为）；
 * 语音经 dev_rs232_voice 旁路 USART6。
 * 上行应答：'1' 主机查询回固定「正常」帧、0x02 版本号回裸 ASCII PROGRAM_CODE，其余命令单向不回。
 */

#include "app_gz_proto_cmd.h"
#include "app_gz_proto_parse.h"
#include "app_boot.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_gz_proto_voice.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"
#include "text_cvt.h"

/* 协议颜色索引 0/1/2 → 显示颜色（红/绿/黄） */
static const display_color_t s_gz_color_map[] = {
    [0] = COLOR_RED,
    [1] = COLOR_GREEN,
    [2] = COLOR_YELLOW,
};

/* 协议亮度 1~5 → 硬件亮度；最大档映射到 DEV_DISPLAY_BRIGHTNESS_MAX(8) */
static const uint8_t s_gz_brightness_map[] = {3, 4, 5, 7, 8};

/* 协议音量 1~5 → 语音板音量值 {1,3,5,7,9}（按设备实测行为，语音板音量非线性档位；
 * STD dev_rs232_voice_volume 直通发送音量值，青海 identity 映射不可复用） */
static const uint8_t s_gz_volume_map[] = {1, 3, 5, 7, 9};

/**
 * @brief  将协议颜色索引转换为显示颜色。
 * @param  idx  协议颜色索引（0 红 / 1 绿 / 2 黄）。
 * @return 对应的显示颜色；越界回退绿色。
 */
static display_color_t gz_map_color(uint8_t idx)
{
    return (idx < 3) ? s_gz_color_map[idx] : COLOR_GREEN;
}

/**
 * @brief  清空整屏（置黑并提交）。
 */
static void _gz_clear_screen(void)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  清空第 row 行区域（行高 FONT_16）。
 * @param  d    显示设备实例。
 * @param  row  行号（0 起）。
 */
static void _gz_clear_row(dev_display_t *d, uint8_t row)
{
    dev_display_fill(d, 0, (uint16_t)row * FONT_16, d->screen_rows, FONT_16, COLOR_BLACK);
}

/**
 * @brief  渲染单行 UTF-8 行文本（'6' 固定格式构造行文用）：先清行再渲染。
 * @param  row    行号（0 起）。
 * @param  color  显示颜色。
 * @param  text   UTF-8 行文本（NUL 结尾）。
 */
static void _gz_render_utf8_line(uint8_t row, display_color_t color, const char *text)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    _gz_clear_row(d, row);
    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = (uint16_t)row * FONT_16,
        .w     = d->screen_rows,
        .h     = FONT_16,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false, /* 单行：超出行宽截断 */
        },
        .color     = color,
        .text      = text,
        .len       = (uint16_t)strlen(text),
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_UTF8,
    });
}

/**
 * @brief  渲染单行 GBK 原始文本（'6' 信息1 原文用）：先清行再渲染。
 * @param  row    行号（0 起）。
 * @param  color  显示颜色。
 * @param  text   GBK/ASCII 文本。
 * @param  len    文本字节数。
 * @note   空行或首字节为空格则该行不显示（协议文档原文）；超过 64 字节截断。
 */
static void _gz_render_gbk_line(uint8_t row, display_color_t color, const uint8_t *text,
                                uint16_t len)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    _gz_clear_row(d, row);
    if (text == NULL || len == 0U || text[0] == ' ')
        return;
    if (len > 64U)
        len = 64U;
    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = (uint16_t)row * FONT_16,
        .w     = d->screen_rows,
        .h     = FONT_16,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false,
        },
        .color     = color,
        .text      = (const char *)text,
        .len       = len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行主机查询（'1' 命令）：回固定「状态正常」应答。
 * @param  ch  源通道（应答回源通道；设备实测经固定串口属实现瑕疵，不沿袭）。
 * @note   协议定义正常/异常两种应答，本实现仅回「正常」一种（按设备实测行为）。
 */
static void _gz_exec_host_query(channel_t *ch)
{
    static const uint8_t rsp_ok[] = {'{', '1', 0x01, 0x00, '}'};
    channel_send(ch, (uint8_t *)rsp_ok, sizeof(rsp_ok));
}

/**
 * @brief  执行自检（'2' 命令）：黄色全屏填充 + 语音「系统正在加电自检」。
 * @note   协议文档原文为固定汉字信息及数字 1~9 交替显示；本实现按裁决
 *         黄色全屏 + 自检语音执行。
 */
static void _gz_exec_self_check(void)
{
    dev_display_t *d = dev_display_get();
    if (d) {
        dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_YELLOW);
        dev_display_commit_frame(d);
    }
    /* 语音「系统正在加电自检」（UTF-8 字面量，语音板要求 GBK，运行时转换） */
    static const char text[] = "系统正在加电自检";
    uint8_t gbk[32];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(text, (uint32_t)(sizeof(text) - 1U), (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  执行单行显示（'3' 命令：第 row+1 行按颜色渲染文本）。
 * @param  p  单行显示参数。
 * @note   先整行清黑再渲染收到内容（文本短于行宽时旧内容残留必须清除）。
 */
static void _gz_exec_one_line(const gz_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    _gz_clear_row(d, p->row);

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = (uint16_t)p->row * FONT_16,
        .w     = d->screen_rows,
        .h     = FONT_16,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false, /* 单行：超出行宽截断 */
        },
        .color     = gz_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行全屏可编辑显示（'4' 命令：按协议 X/Y 坐标渲染，自动换行）。
 * @param  p  全屏显示参数。
 * @note   先整屏清黑再渲染；文本中 0x0A 由渲染引擎换行、0x0D 被跳过。
 *         X/Y 坐标按协议文档原文取参数渲染（设备实测行为为校验坐标后恒按
 *         (0,0) 渲染，不沿袭）。
 */
static void _gz_exec_full_screen(const gz_full_screen_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = p->x,
        .y     = p->y,
        .w     = d->screen_rows,
        .h     = d->screen_cols,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = true,
        },
        .color     = gz_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行全屏点亮（0x01 命令：整屏填充红/绿/黄）。
 * @param  color  归一化颜色索引 0 红 / 1 绿 / 2 黄。
 * @note   03=黄色按协议文档原文；设备实测行为为蓝色，不沿袭。
 */
static void _gz_exec_fill_all(uint8_t color)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, gz_map_color(color));
    dev_display_commit_frame(d);
}

/**
 * @brief  执行获取版本号（0x02 命令）：串口回裸 ASCII PROGRAM_CODE，
 *         屏幕红字全屏显示「版本:PROGRAM_CODE」。
 * @param  ch  源通道。
 * @note   屏幕显示版本为对齐设备实测行为的附加行为（协议文档仅要求返回版本号）。
 */
static void _gz_exec_version(channel_t *ch)
{
    /* 裸 ASCII 应答产品程序编码 PROGRAM_CODE */
    channel_send(ch, (uint8_t *)PROGRAM_CODE, sizeof(PROGRAM_CODE) - 1U);

    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    char text[40];
    int len = snprintf(text, sizeof(text), "版本:%s", PROGRAM_CODE);
    if (len <= 0 || len >= (int)sizeof(text))
        return;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = 0,
        .w     = d->screen_rows,
        .h     = d->screen_cols,
        .style = &(render_style_t){
            .h_align   = ALIGN_CENTER,
            .v_align   = ALIGN_CENTER,
            .word_wrap = false,
        },
        .color     = COLOR_RED,
        .text      = text,
        .len       = (uint16_t)len,
        .font_size = FONT_SELF_ADAPT,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_UTF8,
    });
}

/**
 * @brief  执行亮度设定（'8' 命令：'0' 自动调光 / '1'~'5' 手动档）。
 * @param  level  协议亮度档位（0~5）。
 * @note   自动档恢复光敏任务；手动档挂起光敏任务并映射硬件亮度
 *         {3,4,5,7,8}（协议最大档对应硬件最亮档）。
 */
static void _gz_exec_brightness(uint8_t level)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    if (level == 0) {
        /* 自动调节亮度：恢复光敏任务（若此前被手动档挂起） */
        if (g_light_sensor_task_handle != nullptr)
            osThreadResume(g_light_sensor_task_handle);
        return;
    }
    if (level < 1 || level > 5)
        return;
    if (g_light_sensor_task_handle != nullptr)
        osThreadSuspend(g_light_sensor_task_handle); /* 手动设定 → 关闭自动调光 */
    dev_display_set_brightness(d, s_gz_brightness_map[level - 1]);
}

/**
 * @brief  执行语音音量设定（'9' 命令：'1'~'5' 档）。
 * @param  level  协议音量档位（1~5）。
 * @note   档位来源按设备实测行为：协议 '1'~'5' → 语音板音量值 1/3/5/7/9。
 */
static void _gz_exec_volume(uint8_t level)
{
    if (level < 1 || level > 5)
        return;
    dev_rs232_voice_volume(s_gz_volume_map[level - 1]);
}

/**
 * @brief  执行外设控制（'A' 命令：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 * @param  ctrl  控制位图。
 * @note   车道灯为单灯互斥（true=绿/false=红）：红绿同置时红灯优先。
 */
static void _gz_exec_peripheral(uint8_t ctrl)
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
 * @brief  构造「前缀:%u.%02u 单位」行文本（金额/余额/总重/超重共用）。
 * @param  buf     输出缓冲（≥32B）。
 * @param  bufsz   输出缓冲大小。
 * @param  prefix  行文前缀（如 "金额:"）。
 * @param  fen     数值（分/吨分，整数运算，无浮点）。
 * @param  unit    行文单位（如 "元"/"吨"）。
 * @return 写入字节数（不含 NUL）；截断时 ≥bufsz。
 */
static int _gz_format_value(char *buf, size_t bufsz, const char *prefix, uint32_t fen,
                            const char *unit)
{
    return snprintf(buf, bufsz, "%s%u.%02u %s", prefix, (unsigned)(fen / 100U),
                    (unsigned)(fen % 100U), unit);
}

/**
 * @brief  渲染总重/超重行（'6' 货车）：<0.1 吨该行不显示（空行）。
 * @param  row    行号（0 起）。
 * @param  color  显示颜色。
 * @param  fen    重量（吨分）。
 * @param  prefix 行文前缀（"总重:"/"超重:"）。
 */
static void _gz_render_weight_line(uint8_t row, display_color_t color, uint32_t fen,
                                   const char *prefix)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    if (fen < 10U) {
        _gz_clear_row(d, row); /* <0.1 吨 → 该行不显示（空行） */
        return;
    }
    char line[32];
    int len = _gz_format_value(line, sizeof(line), prefix, fen, "吨");
    if (len > 0 && len < (int)sizeof(line))
        _gz_render_utf8_line(row, color, line);
}

/**
 * @brief  执行固定格式显示（'6' 命令）。
 * @param  p  固定格式参数。
 * @note   行文对齐设备实测行为（协议文档原文为「车型: X型」等全角冒号形式）。
 *         客车：车型/金额/余额/信息1 四行，信息2 不显示（按设备实测行为）；
 *         货车：有超重(≥0.1 吨)→金额/余额/总重/超重；无超重→车型/金额/余额/总重。
 *         金额 ≥0.5 元同步播报费额语音。
 */
static void _gz_exec_fixed(const gz_fixed_t *p)
{
    /* '|' 分隔字段拆分（末字段到参数区结尾，不原地改写帧数据） */
    const uint8_t *fields[5] = {0};
    uint16_t flen[5]         = {0};
    int n                    = 0;
    const uint8_t *cur       = p->raw;
    const uint8_t *end       = p->raw + p->raw_len;
    while (cur < end && n < 5) {
        const uint8_t *sep = memchr(cur, '|', (size_t)(end - cur));
        fields[n]          = cur;
        flen[n]            = (uint16_t)((sep != NULL ? sep : end) - cur);
        n++;
        if (sep == NULL)
            break;
        cur = sep + 1;
    }

    /* 数值字段 ASCII 数字串转分/吨分（整数运算，无浮点） */
    uint32_t type_fen    = (n > 0) ? gz_amount_to_fen(fields[0], flen[0]) : 0U;
    uint32_t amount_fen  = (n > 1) ? gz_amount_to_fen(fields[1], flen[1]) : 0U;
    uint32_t balance_fen = (n > 2) ? gz_amount_to_fen(fields[2], flen[2]) : 0U;
    uint32_t weight_fen  = (n > 3) ? gz_amount_to_fen(fields[3], flen[3]) : 0U;
    uint32_t exceed_fen  = (n > 4) ? gz_amount_to_fen(fields[4], flen[4]) : 0U;

    display_color_t color = gz_map_color(p->color);
    char line[32];
    int len;

    if (p->type == 0U) { /* 客车 */
        /* 行0 车型 */
        len = snprintf(line, sizeof(line), "车型:   %u    型", (unsigned)(type_fen / 100U));
        if (len > 0 && len < (int)sizeof(line))
            _gz_render_utf8_line(0, color, line);
        /* 行1 金额 */
        len = _gz_format_value(line, sizeof(line), "金额:", amount_fen, "元");
        if (len > 0 && len < (int)sizeof(line))
            _gz_render_utf8_line(1, color, line);
        /* 行2 余额 */
        len = _gz_format_value(line, sizeof(line), "余额:", balance_fen, "元");
        if (len > 0 && len < (int)sizeof(line))
            _gz_render_utf8_line(2, color, line);
        /* 行3 信息1 原文（GBK；空行或首字节空格不显示） */
        if (n > 3)
            _gz_render_gbk_line(3, color, fields[3], flen[3]);
        /* 行4 信息2 不显示（按设备实测行为） */
    } else { /* 货车 */
        if (exceed_fen >= 10U) {
            /* 有超重（≥0.1 吨）：金额/余额/总重/超重 */
            len = _gz_format_value(line, sizeof(line), "金额:", amount_fen, "元");
            if (len > 0 && len < (int)sizeof(line))
                _gz_render_utf8_line(0, color, line);
            len = _gz_format_value(line, sizeof(line), "余额:", balance_fen, "元");
            if (len > 0 && len < (int)sizeof(line))
                _gz_render_utf8_line(1, color, line);
            _gz_render_weight_line(2, color, weight_fen, "总重:");
            _gz_render_weight_line(3, color, exceed_fen, "超重:");
        } else {
            /* 无超重：车型/金额/余额/总重 */
            len = snprintf(line, sizeof(line), "车型:   %u    型", (unsigned)(type_fen / 100U));
            if (len > 0 && len < (int)sizeof(line))
                _gz_render_utf8_line(0, color, line);
            len = _gz_format_value(line, sizeof(line), "金额:", amount_fen, "元");
            if (len > 0 && len < (int)sizeof(line))
                _gz_render_utf8_line(1, color, line);
            len = _gz_format_value(line, sizeof(line), "余额:", balance_fen, "元");
            if (len > 0 && len < (int)sizeof(line))
                _gz_render_utf8_line(2, color, line);
            _gz_render_weight_line(3, color, weight_fen, "总重:");
        }
    }

    /* 金额 ≥0.5 元播报费额语音（0 元不播报，由 gz_voice_fee_amount 内部判定） */
    gz_voice_fee_amount(amount_fen);
}

/**
 * @brief  执行贵州协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 * @note   协议未定义应答机制：除 '1' 主机查询回「正常」帧、0x02 版本号回裸 ASCII
 *         PROGRAM_CODE 外，其余命令单向执行不回；解析失败静默丢弃。
 */
void gz_execute_cmd(channel_t *ch, const gz_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != GZ_PARSE_OK)
        return;

    switch (cmd->cmd) {
        case GZ_PCMD_HOST_QUERY:
            _gz_exec_host_query(ch);
            break;
        case GZ_PCMD_SELF_CHECK:
            _gz_exec_self_check();
            break;
        case GZ_PCMD_ONE_LINE:
            _gz_exec_one_line(&cmd->p.one_line);
            break;
        case GZ_PCMD_FULL_SCREEN:
            _gz_exec_full_screen(&cmd->p.full_screen);
            break;
        case GZ_PCMD_CLEAR:
            _gz_clear_screen();
            break;
        case GZ_PCMD_FIXED_FORMAT:
            _gz_exec_fixed(&cmd->p.fixed);
            break;
        case GZ_PCMD_CIVIL_VOICE:
            gz_voice_civil(cmd->p.civil.idx);
            break;
        case GZ_PCMD_BRIGHTNESS:
            _gz_exec_brightness(cmd->p.brightness.level);
            break;
        case GZ_PCMD_VOLUME:
            _gz_exec_volume(cmd->p.volume.level);
            break;
        case GZ_PCMD_PERIPHERAL:
            _gz_exec_peripheral(cmd->p.peripheral.ctrl);
            break;
        case GZ_PCMD_FEE_VOICE:
            gz_voice_fee_amount(cmd->p.fee.amount_fen);
            break;
        case GZ_PCMD_FILL_ALL:
            _gz_exec_fill_all(cmd->p.fill_all.color);
            break;
        case GZ_PCMD_VERSION:
            _gz_exec_version(ch);
            break;
        default:
            break;
    }
}