/**
 * @file    app_yn_proto_cmd.c
 * @brief   云南常规费显协议命令执行
 *
 * 显示内容映射到 app_render（协议文本 GBK 直通渲染，24 点阵 FONT_24 行高 24px——
 * 协议原文 24 点阵、一行 12 ASCII / 6 汉字，本设备 24 点阵一行可容纳
 * 16 ASCII / 8 汉字，超宽由渲染引擎截断；行号 '1'~'5' 全按协议接受，
 * 行 5 在 96px 屏高下「执行但不落屏」，见用户决定 2）；
 * 灯控映射到 dev_io_lane_light / dev_io_flash_light（红优先，与贵州裁决一致）；
 * 亮度 0x00=恢复光敏自动调光、'1'~'8'=挂起光敏 + 硬件档恒等映射
 * （8 最亮，用户决定 5）；
 * 自检 '2' 复用出厂测试「老化循环显示」序列 + 每 5s 播报「系统正在自检」，
 * 可被下一帧命令打断（用户决定 3）；
 * 0x01 全屏点亮支持 01红/02绿/03黄/04蓝/05紫/06青/07白（用户决定 10）；
 * 音量 '1'~'5' 映射语音板音量 {1,3,5,7,9}（沿贵州实测映射，同一语音板）；
 * 语音经 dev_rs232_voice 旁路 USART6。
 * 上行应答：'1' 主机查询回「正常」帧、0x02 版本号回裸 ASCII PROGRAM_CODE
 * （用户决定 7），其余命令单向不回。
 */

#include "app_yn_proto_cmd.h"
#include "app_yn_proto_parse.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_yn_proto_voice.h"
#include "app_boot.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"
#include "text_cvt.h"

/** @brief 单行行高（协议 24 点阵） */
#define YN_ROW_PX (FONT_24)

/* 协议颜色索引 0/1/2 → 显示颜色（红/绿/黄） */
static const display_color_t s_yn_color_map[] = {
    [0] = COLOR_RED,
    [1] = COLOR_GREEN,
    [2] = COLOR_YELLOW,
};

/* 协议音量 1~5 → 语音板音量值 {1,3,5,7,9}（沿贵州实测映射：语音板音量非线性档位，
 * STD dev_rs232_voice_volume 直通发送音量值） */
static const uint8_t s_yn_volume_map[] = {1, 3, 5, 7, 9};

/** @brief 自检任务运行标志（防重复触发，任务内自清） */
static volatile bool s_yn_selftest_running = false;

/** @brief 自检中止标志：下一帧命令到达时置位，自检任务检测后退出（用户决定 3） */
static volatile bool s_yn_selftest_abort = false;

/* ---- 老化循环显示序列复用（用户决定 3，2026-08-24 修订）----
 * 复用 Application/Src/app_factory_test.c 263-299 行「老化循环显示」逻辑的
 * 显示序列与实现方式：整屏单字居中全屏显示（清黑 + 渲染 + commit），
 * 字号 {FONT_16, FONT_24, FONT_32} × 字体 {ST, FS, KT, HT} 循环推进，
 * 文本逐字显示「重庆创迪科技发展有限公司设备老化测试」，循环直至被打断。
 * 与出厂测试的差异：不切换车道灯（避免污染 'A' 外设命令的灯状态）、
 * 不做按键等待（固定步进延时 + 中止标志分片检查）、循环而非单轮；
 * 出厂测试自身的任务/状态（monitor、中止标志、老化亮度）完全不动。 */
#define YN_SELFTEST_TEXT "重庆创迪科技发展有限公司设备老化测试"

static const font_size_t s_yn_selftest_sizes[] = {
    FONT_16,
    FONT_24,
    FONT_32,
};
static const font_type_t s_yn_selftest_types[] = {
    FONT_ST,
    FONT_FS,
    FONT_KT,
    FONT_HT,
};
#define YN_SELFTEST_SIZE_COUNT (sizeof(s_yn_selftest_sizes) / sizeof(s_yn_selftest_sizes[0]))
#define YN_SELFTEST_TYPE_COUNT (sizeof(s_yn_selftest_types) / sizeof(s_yn_selftest_types[0]))

/** @brief 自检单字步进时长（老化 3s/字的精简节奏，协议未指定） */
#define YN_SELFTEST_CHAR_MS (500U)
/** @brief 语音播报周期：每 5s 播报一次「系统正在自检」（用户决定 3） */
#define YN_SELFTEST_VOICE_MS (5000U)
/** @brief 中止检查分片（延时细分粒度，保证打断响应 ≤100ms） */
#define YN_SELFTEST_ABORT_SLICE_MS (100U)

/**
 * @brief  自检单字全屏显示（复现 app_factory_test.c _aging_fill_screen 的实现方式）。
 * @param  d         显示设备实例。
 * @param  size      字号（FONT_16/FONT_24/FONT_32）。
 * @param  type      字体（FONT_ST/FONT_FS/FONT_KT/FONT_HT）。
 * @param  ch_utf8   单字 UTF-8 字面量。
 * @param  ch_len    单字字节数（ASCII 1 / 汉字 3）。
 */
static void _yn_selftest_fill_char(dev_display_t *d, font_size_t size, font_type_t type,
                                   const char *ch_utf8, uint8_t ch_len)
{
    uint8_t cols = d->screen_cols / size;
    uint8_t rows = d->screen_rows / size;
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    d->dirty = false;

    static char buf[256];
    uint16_t pos   = 0;
    uint16_t count = cols * rows;
    while (count--) {
        memcpy(buf + pos, ch_utf8, ch_len);
        pos += ch_len;
    }

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = d->screen_rows,
        .h         = d->screen_cols,
        .color     = COLOR_WHITE,
        .text      = buf,
        .len       = pos,
        .font_size = size,
        .font_type = type,
        .text_enc  = FONT_ENC_UTF8,
        .style     = &(render_style_t){
            .h_align   = ALIGN_CENTER,
            .v_align   = ALIGN_CENTER,
            .word_wrap = true,
        },
    });
    dev_display_commit_frame(d);
}

/**
 * @brief  播报「系统正在自检」（UTF-8 → GBK → 语音板 TTS，5s 周期调用）。
 */
static void _yn_selftest_voice(void)
{
    static const char voice_text[] = "系统正在自检";
    uint8_t gbk[32];
    uint32_t gbk_len = sizeof(gbk);
    UTF8ToGBK(voice_text, (uint32_t)(sizeof(voice_text) - 1U), (char *)gbk, &gbk_len);
    dev_rs232_voice_play(gbk, (uint16_t)gbk_len);
}

/**
 * @brief  自检任务：老化循环显示序列 + 每 5s 播报「系统正在自检」（用户决定 3）。
 * @param  argument  任务参数，当前未使用。
 * @note   循环显示直至被下一帧命令打断（yn_execute_cmd 置 s_yn_selftest_abort）；
 *         被打断退出时**不清屏**——屏幕交给打断它的新命令（清屏会抹掉其渲染）。
 *         防重入：s_yn_selftest_running 由 _yn_exec_self_check 置位、任务内自清。
 */
static void _yn_selftest_task(void *argument)
{
    (void)argument;

    dev_display_t *d = dev_display_get();

    uint32_t voice_elapsed = 0; /* 距上次语音播报的累计时长（ms） */
    for (uint8_t type_idx = 0; !s_yn_selftest_abort;
         type_idx = (type_idx + 1) % YN_SELFTEST_TYPE_COUNT) {
        for (uint8_t size_idx = 0; size_idx < YN_SELFTEST_SIZE_COUNT; size_idx++) {
            font_size_t fsize = s_yn_selftest_sizes[size_idx];
            if (d != nullptr && (fsize > d->screen_rows || fsize > d->screen_cols))
                continue;

            const char *ch_ptr = YN_SELFTEST_TEXT;
            while (*ch_ptr) {
                uint8_t ch_len    = ((uint8_t)*ch_ptr >= 0xE0) ? 3 : 1;
                char single_ch[4] = {ch_ptr[0], ch_len > 1 ? ch_ptr[1] : 0,
                                     ch_len > 2 ? ch_ptr[2] : 0, 0};

                if (d != nullptr)
                    _yn_selftest_fill_char(d, fsize, s_yn_selftest_types[type_idx],
                                           single_ch, ch_len);

                /* 每 5s 播报一次「系统正在自检」：与循环显示并行
                 * （dev_rs232_voice_play 非阻塞发送，不打断显示步进） */
                if (voice_elapsed >= YN_SELFTEST_VOICE_MS) {
                    voice_elapsed = 0;
                    _yn_selftest_voice();
                }

                /* 分片延时：控制显示步进节奏 + 及时响应下一帧命令的中止 */
                for (uint32_t ms = 0; ms < YN_SELFTEST_CHAR_MS && !s_yn_selftest_abort;
                     ms += YN_SELFTEST_ABORT_SLICE_MS) {
                    osDelay(YN_SELFTEST_ABORT_SLICE_MS);
                }
                voice_elapsed += YN_SELFTEST_CHAR_MS;
                if (s_yn_selftest_abort)
                    break;
                ch_ptr += ch_len;
            }
            if (s_yn_selftest_abort)
                break;
        }
        if (s_yn_selftest_abort)
            break;
    }

    s_yn_selftest_running = false;
    s_yn_selftest_abort   = false;
    osThreadExit();
}

/**
 * @brief  执行自检（'2' 命令）：启动老化循环显示任务。
 * @note   一次性任务执行（协议「系统进入自动检测状态」）；任务进行中重复
 *         触发被忽略（防重入）；任务可被下一帧命令打断（用户决定 3）。
 */
static void _yn_exec_self_check(void)
{
    if (s_yn_selftest_running)
        return;
    s_yn_selftest_running = true; /* 先置标志防重入；任务结束时自清 */
    s_yn_selftest_abort   = false;

    static const osThreadAttr_t s_yn_selftest_attr = {
        .name       = "yn_selftest_task",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    osThreadNew(_yn_selftest_task, NULL, &s_yn_selftest_attr);
}

/**
 * @brief  将协议颜色索引转换为显示颜色。
 * @param  idx  协议颜色索引（0 红 / 1 绿 / 2 黄）。
 * @return 对应的显示颜色；越界回退绿色。
 */
static display_color_t yn_map_color(uint8_t idx)
{
    return (idx < 3) ? s_yn_color_map[idx] : COLOR_GREEN;
}

/**
 * @brief  清空整屏（置黑并提交）。
 */
static void _yn_clear_screen(void)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  清空第 row 行区域（行高 YN_ROW_PX=24）。
 * @param  d    显示设备实例。
 * @param  row  行号（0 起）。
 * @note   用户决定 2（2026-08-24 修订）：行号 '1'~'5' 全按协议执行，
 *         不丢弃、不拒绝。行高 24px 下 96px 屏高只容纳 4 行——
 *         行 5（row 4，y=96）越界时 dev_display_fill 起点越界早退
 *         （安全无副作用），即「执行但不落屏」。
 */
static void _yn_clear_row(dev_display_t *d, uint8_t row)
{
    dev_display_fill(d, 0, (uint16_t)row * YN_ROW_PX, d->screen_rows, YN_ROW_PX, COLOR_BLACK);
}

/**
 * @brief  执行主机查询（'1' 命令）：回固定「状态正常」应答帧 7B 31 01 00 7D。
 * @param  ch  源通道（应答回源通道）。
 * @note   协议定义正常/异常两种应答，本实现仅回「正常」一种（设备恒正常，
 *         异常应答无触发条件）。
 */
static void _yn_exec_host_query(channel_t *ch)
{
    static const uint8_t rsp_ok[] = {'{', '1', 0x01, 0x00, '}'};
    channel_send(ch, (uint8_t *)rsp_ok, sizeof(rsp_ok));
}

/**
 * @brief  执行单行显示（'3' 命令：第 row+1 行按颜色渲染文本）。
 * @param  p  单行显示参数。
 * @note   先整行清黑再渲染收到内容（文本短于行宽时旧内容残留必须清除）；
 *         FONT_24 渲染、GBK 直通、超出行宽截断。
 *         用户决定 2（2026-08-24 修订）：行 5（row 4，y=96）渲染调用照常
 *         发出——渲染层 dev_display_fill / dev_display_draw_bitmap 起点越界
 *         早退，天然不落屏（安全无副作用），命令本身不丢弃、不拒绝。
 */
static void _yn_exec_one_line(const yn_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    _yn_clear_row(d, p->row);

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = (uint16_t)p->row * YN_ROW_PX,
        .w     = d->screen_rows,
        .h     = YN_ROW_PX,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false, /* 单行：超出行宽截断 */
        },
        .color     = yn_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_24,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行全屏可编辑显示（'4' 命令：按协议 X/Y 坐标渲染，自动换行）。
 * @param  p  全屏显示参数。
 * @note   先整屏清黑再渲染；文本中 0x0A 0x0D / 0x0D 0x0A 回车由渲染引擎
 *         处理（0x0A 换行、0x0D 跳过）。X/Y 坐标按协议原文取参数渲染。
 *         坐标越界（x≥屏宽或 y≥屏高）时丢弃渲染（清屏仍执行）。
 */
static void _yn_exec_full_screen(const yn_full_screen_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);

    if (p->x >= d->screen_rows || p->y >= d->screen_cols)
        return; /* 坐标越界：清屏后不渲染 */

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = p->x,
        .y     = p->y,
        .w     = (uint16_t)(d->screen_rows - p->x),
        .h     = (uint16_t)(d->screen_cols - p->y),
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = true,
        },
        .color     = yn_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_24,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行全屏点亮（0x01 命令：整屏填充单色）。
 * @param  color  DATA0 二进制颜色值，与 display_color_t 枚举值恒等：
 *                0x01 红 / 0x02 绿 / 0x03 黄（协议原文三色）/
 *                0x04 蓝 / 0x05 紫 / 0x06 青 / 0x07 白
 *                （用户决定 10 扩展取值；P5 户外全彩屏支持 8 色，
 *                黑色除外——全屏黑等效 '5' 全屏清除）。
 */
static void _yn_exec_fill_all(uint8_t color)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    if (color < 1 || color > 7)
        return; /* 防御：仅接受 COLOR_RED~COLOR_WHITE */
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, (display_color_t)color);
    dev_display_commit_frame(d);
}

/**
 * @brief  执行获取版本号（0x02 命令）：回裸 ASCII 固件 PROGRAM_CODE。
 * @param  ch  源通道。
 * @note   用户决定 7（2026-08-24 修订）：不再回硬编码 "YN_FX_P5_1.0"，
 *         与山东/贵州同模式回编译期常量 PROGRAM_CODE（app_boot.h）。
 */
static void _yn_exec_version(channel_t *ch)
{
    channel_send(ch, (uint8_t *)PROGRAM_CODE, sizeof(PROGRAM_CODE) - 1U);
}

/**
 * @brief  执行亮度设定（'8' 命令，用户决定 5，2026-08-24 修订）。
 * @param  level  0 = 自动亮度（协议参数 NUL 字节 0x00）；
 *                1~8 = 手动亮度档（协议参数 ASCII '1'~'8'，8 最亮）。
 * @note   0x00 → 恢复光敏任务自动调光（osThreadResume，替代此前
 *         「恢复自动调光需复位」的临时方案）；
 *         1~8 → 挂起光敏任务（防 1s 周期自动调光覆盖手动档）+
 *         硬件档恒等映射（DEV_DISPLAY_BRIGHTNESS_MAX=8）。
 */
static void _yn_exec_brightness(uint8_t level)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    if (level == 0) {
        /* 自动亮度：恢复光敏任务（此前可能被手动档挂起） */
        if (g_light_sensor_task_handle != nullptr)
            osThreadResume(g_light_sensor_task_handle);
        return;
    }

    if (level > 8)
        return;
    if (g_light_sensor_task_handle != nullptr)
        osThreadSuspend(g_light_sensor_task_handle); /* 手动设定 → 关闭自动调光 */
    dev_display_set_brightness(d, level);            /* 协议档位与硬件档位恒等映射 */
}

/**
 * @brief  执行语音音量设定（'9' 命令：'1'~'5' 档）。
 * @param  level  协议音量档位（1~5）。
 * @note   档位映射沿贵州实测：协议 '1'~'5' → 语音板音量值 1/3/5/7/9。
 */
static void _yn_exec_volume(uint8_t level)
{
    if (level < 1 || level > 5)
        return;
    dev_rs232_voice_volume(s_yn_volume_map[level - 1]);
}

/**
 * @brief  执行外设控制（'A' 命令：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 * @param  ctrl  控制位图。
 * @note   车道灯为单灯互斥（true=绿/false=红）：红绿同置时红灯优先
 *         （沿贵州裁决，协议未定义优先级）。
 */
static void _yn_exec_peripheral(uint8_t ctrl)
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
 * @brief  执行收费金额语音播放（'B' 命令）。
 * @param  amount_fen  收费金额，单位分。
 * @note   0 元不播报；小数播报小数（123.4→「123.4」，末位 0 剔除）。
 */
static void _yn_exec_fee_voice(uint32_t amount_fen)
{
    yn_voice_fee_amount(amount_fen);
}

/**
 * @brief  执行云南协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 * @note   除 '1' 主机查询回「正常」帧、0x02 版本号回裸 ASCII YN_FX_P5_1.0 外，
 *         其余命令单向执行不回；解析失败静默丢弃（非法参数不崩溃）。
 */
void yn_execute_cmd(channel_t *ch, const yn_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != YN_PARSE_OK)
        return;

    /* 自检打断（用户决定 3，2026-08-24 修订）：自检进行中收到其他有效命令 →
     * 置中止标志，自检任务在下一分片（≤100ms）检测到后退出且不清屏，
     * 屏幕交给本命令；'2' 自检本身不打断（防重入由 _yn_exec_self_check 处理）。 */
    if (s_yn_selftest_running && cmd->cmd != YN_PCMD_SELF_CHECK)
        s_yn_selftest_abort = true;

    switch (cmd->cmd) {
        case YN_PCMD_HOST_QUERY:
            _yn_exec_host_query(ch);
            break;
        case YN_PCMD_SELF_CHECK:
            _yn_exec_self_check();
            break;
        case YN_PCMD_ONE_LINE:
            _yn_exec_one_line(&cmd->p.one_line);
            break;
        case YN_PCMD_FULL_SCREEN:
            _yn_exec_full_screen(&cmd->p.full_screen);
            break;
        case YN_PCMD_CLEAR:
            _yn_clear_screen();
            break;
        case YN_PCMD_CLEAR_ROW: {
            dev_display_t *d = dev_display_get();
            if (d != nullptr)
                _yn_clear_row(d, cmd->p.clear_row.row);
            break;
        }
        case YN_PCMD_CIVIL_VOICE:
            yn_voice_civil(cmd->p.civil.idx);
            break;
        case YN_PCMD_BRIGHTNESS:
            _yn_exec_brightness(cmd->p.brightness.level);
            break;
        case YN_PCMD_VOLUME:
            _yn_exec_volume(cmd->p.volume.level);
            break;
        case YN_PCMD_PERIPHERAL:
            _yn_exec_peripheral(cmd->p.peripheral.ctrl);
            break;
        case YN_PCMD_FEE_VOICE:
            _yn_exec_fee_voice(cmd->p.fee.amount_fen);
            break;
        case YN_PCMD_FILL_ALL:
            _yn_exec_fill_all(cmd->p.fill_all.color);
            break;
        case YN_PCMD_VERSION:
            _yn_exec_version(ch);
            break;
        default:
            break;
    }
}