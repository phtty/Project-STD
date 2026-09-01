/**
 * @file    app_sd_proto_cmd.c
 * @brief   山东车道费额显示器通信协议命令执行
 *
 * 显示内容映射到 app_render（GBK 直通渲染，行高 FONT_16）；
 * 灯控映射到 dev_io_lane_light / dev_io_flash_light（PD14/PD15）；
 * 亮度 0=自动调光（恢复光敏任务），1~5 映射硬件档 {3,4,5,7,8}。
 * 协议未定义上行应答（除 '2' 取版本号回裸 ASCII 产品程序编码 PROGRAM_CODE），
 * 其余命令单向不回。
 */

#include "app_sd_proto_cmd.h"
#include "app_boot.h"
#include "app_dispatch.h"

#include <stdint.h>

#include "app_light_sensor.h"
#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"

/* 协议颜色索引 0/1/2 → 显示颜色（红/绿/黄） */
static const display_color_t s_sd_color_map[] = {
    [0] = COLOR_RED,
    [1] = COLOR_GREEN,
    [2] = COLOR_YELLOW,
};

/* 协议亮度 1~5 → 硬件亮度；最大档映射到 DEV_DISPLAY_BRIGHTNESS_MAX(8) */
static const uint8_t s_sd_brightness_map[] = {3, 4, 5, 7, 8};

/**
 * @brief  将协议颜色索引转换为显示颜色。
 * @param  idx  协议颜色索引（0 红 / 1 绿 / 2 黄）。
 * @return 对应的显示颜色；越界回退绿色。
 */
static display_color_t sd_map_color(uint8_t idx)
{
    return (idx < 3) ? s_sd_color_map[idx] : COLOR_GREEN;
}

/**
 * @brief  清空整屏（置黑并提交）。
 */
static void _sd_clear_screen(void)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);
}

/**
 * @brief  执行全屏单色显示（'1' 命令：整屏填充红/绿/黄）。
 * @param  color  归一化颜色索引 0 红 / 1 绿 / 2 黄。
 */
static void _sd_exec_fill_all(uint8_t color)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;
    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, sd_map_color(color));
    dev_display_commit_frame(d);
}

/**
 * @brief  执行单行显示（'3' 命令：第 row+1 行按颜色渲染文本）。
 * @param  p  单行显示参数。
 * @note   先整行清黑再渲染收到内容（文本短于行宽时旧内容残留必须清除）。
 */
static void _sd_exec_one_line(const sd_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, (uint16_t)p->row * FONT_16, d->screen_rows, FONT_16,
                     COLOR_BLACK);

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
        .color     = sd_map_color(p->color),
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
 */
static void _sd_exec_full_screen(const sd_full_screen_t *p)
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
        .color     = sd_map_color(p->color),
        .text      = (const char *)p->text,
        .len       = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_GBK,
    });
}

/**
 * @brief  执行亮度设定（'7' 命令：'0' 自动调光 / '1'~'5' 手动档）。
 * @param  level  协议亮度档位（0~5）。
 * @note   自动档恢复光敏任务；手动档挂起光敏任务并映射硬件亮度
 *         {3,4,5,7,8}（协议最大档对应硬件最亮档）。
 */
static void _sd_exec_brightness(uint8_t level)
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
    dev_display_set_brightness(d, s_sd_brightness_map[level - 1]);
}

/**
 * @brief  执行外设控制（'8' 命令：bit0 绿灯 / bit1 红灯 / bit2 黄闪报警）。
 * @param  ctrl  控制位图。
 * @note   车道灯为单灯互斥（true=绿/false=红）：红绿同置时红灯优先。
 */
static void _sd_exec_peripheral(uint8_t ctrl)
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
 * @brief  执行山东协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 * @note   协议未定义应答机制：除 '2' 取版本号回裸 ASCII 产品程序编码
 *         PROGRAM_CODE 外，其余命令单向执行不回；解析失败静默丢弃。
 */
void sd_execute_cmd(channel_t *ch, const sd_parsed_cmd_t *cmd)
{
    if (!cmd || cmd->sta != SD_PARSE_OK)
        return;

    switch (cmd->cmd) {
        case SD_PCMD_FILL_ALL:
            _sd_exec_fill_all(cmd->p.fill_all.color);
            break;
        case SD_PCMD_VERSION:
            /* 裸 ASCII 应答产品程序编码 PROGRAM_CODE */
            channel_send(ch, (uint8_t *)PROGRAM_CODE, sizeof(PROGRAM_CODE) - 1U);
            break;
        case SD_PCMD_ONE_LINE:
            _sd_exec_one_line(&cmd->p.one_line);
            break;
        case SD_PCMD_FULL_SCREEN:
            _sd_exec_full_screen(&cmd->p.full_screen);
            break;
        case SD_PCMD_CLEAR:
            _sd_clear_screen();
            break;
        case SD_PCMD_BRIGHTNESS:
            _sd_exec_brightness(cmd->p.brightness.level);
            break;
        case SD_PCMD_PERIPHERAL:
            _sd_exec_peripheral(cmd->p.peripheral.ctrl);
            break;
        default:
            break;
    }
}