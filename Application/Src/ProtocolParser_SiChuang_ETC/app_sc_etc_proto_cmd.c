/**
 * @file    app_sc_etc_proto_cmd.c
 * @brief   四川 ETC 费显协议（1D）命令执行
 *
 * 显示内容映射到 app_render（GBK 直通渲染，行高 FONT_16）；
 * 灯控映射到 dev_io_lane_light / dev_io_flash_light；
 * 上行应答「收到即回」：00 正常 / 01 数据超长 / 02
 * 帧错误；心跳帧不回（文档单向）。
 */

#include "app_sc_etc_proto_cmd.h"
#include "app_dispatch.h"

#include <stdint.h>
#include <string.h>

#include "app_light_sensor.h"
#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "stm32f4xx_hal.h"

/** 每行数据字节数（原 14B/7 字；单行上限 24B/12 字） */
#define SC_ETC_BYTES_PER_LINE (24U)
/** 屏上行数（支持行号 1~6） */
#define SC_ETC_LINE_COUNT (6U)

/** 显示颜色跟踪：显示颜色跟随通行灯状态
 *  （0A 36 红 → 红字；0A 37 绿 → 绿字；上电默认绿）。 */
static display_color_t s_etc_color = COLOR_GREEN;

/* 「ETC车道关闭，请择道行驶」（四川ETC费显协议 §7 心跳机制原文，UTF-8 字面量） */
static const uint8_t s_sc_etc_lane_closed_text[] = "ETC车道关闭，请择道行驶";

/**
 * @brief  发送屏应答帧 `0A XX 0D`。
 * @param  ch     当前通道。
 * @param  code   返回码：00 正常 / 01 超长 / 02 帧错。
 */
static void _sc_etc_send_ack(channel_t *ch, uint8_t code) {
  const uint8_t ack[3] = {0x0A, code, 0x0D};
  channel_send(ch, (uint8_t *)ack, sizeof(ack));
}

/**
 * @brief  清空整屏（置黑并提交）。
 */
static void _sc_etc_clear_screen(void) {
  dev_display_t *d = dev_display_get();
  if (!d)
    return;
  dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
  dev_display_commit_frame(d);
}

/**
 * @brief  渲染一行文本。
 * @param  row    行索引 0~5（对应协议行号 1~6）。
 * @param  text   文本（GBK/ASCII 混合）。
 * @param  len    文本字节数。
 * @note   先整行清黑再渲染收到内容
 *         （文本短于行宽时旧内容残留必须清除）。
 */
static void _sc_etc_render_line(uint8_t row, const uint8_t *text,
                                uint16_t len) {
  dev_display_t *d = dev_display_get();
  if (!d)
    return;

  dev_display_fill(d, 0, (uint16_t)row * FONT_16, d->screen_rows, FONT_16,
                   COLOR_BLACK);

  app_render(&(render_cfg_t){
      .type = RENDER_TEXT,
      .x = 0,
      .y = (uint16_t)row * FONT_16,
      .w = d->screen_rows,
      .h = FONT_16,
      .style =
          &(render_style_t){
              .h_align = ALIGN_LEFT_UP,
              .v_align = ALIGN_LEFT_UP,
              .word_wrap = false,
          },
      .color = s_etc_color, /* 显示颜色跟随通行灯状态 */
      .text = (const char *)text,
      .len = len,
      .font_size = FONT_16,
      .font_type = FONT_ST,
      .text_enc = FONT_ENC_GBK,
  });
}

/**
 * @brief  执行显示命令（静态/滚屏）。
 * @param  p  显示参数。
 * @note   命令语义：
 *          0x20=清屏（行号 0 清全屏，行号 n 清第 n 行）；
 *          0x30=初始化 → 软件复位（NVIC_SystemReset，非清屏）；
 *          全屏数据由字库引擎按屏宽自动换行，
 *          本实现等效为整屏 word_wrap 渲染；滚屏按静态全屏显示。
 */
static void _sc_etc_exec_display(const sc_etc_display_t *p) {
  dev_display_t *d = dev_display_get();
  if (!d)
    return;

  if (p->text_len > 0U) {
    if (p->text[0] == 0x20) { /* 清屏 */
      if (p->row == 0U) {
        _sc_etc_clear_screen();
      } else {
        dev_display_fill(d, 0, (uint16_t)(p->row - 1U) * FONT_16,
                         d->screen_rows, FONT_16, COLOR_BLACK);
        dev_display_commit_frame(d);
      }
      return;
    }
    if (p->text[0] == 0x30) { /* 初始化 → 软件复位 */
      NVIC_SystemReset();
      return;
    }
  }

  if (p->text_len == 0U)
    return;

  if (p->row == 0U) {
    /* 全屏（含滚屏）：先清屏再自第 1 行第 1 列按屏宽自动换行渲染，
     * 变长数据（≤145B）按收到内容渲染，无固定 56B 布局 */
    _sc_etc_clear_screen();
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT,
        .x = 0,
        .y = 0,
        .w = d->screen_rows,
        .h = d->screen_cols,
        .style =
            &(render_style_t){
                .h_align = ALIGN_LEFT_UP,
                .v_align = ALIGN_LEFT_UP,
                .word_wrap = true,
            },
        .color = s_etc_color,
        .text = (const char *)p->text,
        .len = p->text_len,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc = FONT_ENC_GBK,
    });
  } else {
    _sc_etc_render_line((uint8_t)(p->row - 1U), p->text, p->text_len);
  }
}

/**
 * @brief  执行亮度设置：XX 00~07 → 硬件 1~8 档（YY 保留位忽略）。
 * @param  level  协议亮度值。
 * @note   依据四川ETC费显协议 §6：XX 00~07 亮度，00 为最暗、07 为最亮
 *         （无自动调光档位定义）→ 硬件档 = XX + 1；手动设定时关闭自动调光。
 */
static void _sc_etc_exec_brightness(uint8_t level) {
  dev_display_t *d = dev_display_get();
  if (!d)
    return;
  if (g_light_sensor_task_handle != nullptr)
    osThreadSuspend(g_light_sensor_task_handle); /* 手动设定 → 关闭自动调光 */
  dev_display_set_brightness(
      d, (uint8_t)(level + 1U)); /* 00→1（最暗）… 07→8（最亮） */
}

/**
 * @brief  心跳超时渲染「ETC车道关闭，请择道行驶」（依据四川ETC费显协议 §7）。
 * @note   2026-08-17：唯一调用方 sc_etc_heartbeat_task 已 #if 0 停用，本函数
 *         保留待恢复（无引用，链接期被 --gc-sections 丢弃，不占 Flash）。
 */
void sc_etc_show_lane_closed(void) {
  dev_display_t *d = dev_display_get();
  if (!d)
    return;
  dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
  dev_display_commit_frame(d);

  app_render(&(render_cfg_t){
      .type = RENDER_TEXT,
      .x = 0,
      .y = 0,
      .w = d->screen_rows,
      .h = d->screen_cols,
      .style =
          &(render_style_t){
              .h_align = ALIGN_CENTER,
              .v_align = ALIGN_CENTER,
              .word_wrap = true,
          },
      .color = COLOR_RED,
      .text = (const char *)s_sc_etc_lane_closed_text,
      .len = sizeof(s_sc_etc_lane_closed_text) - 1U,
      .font_size = FONT_SELF_ADAPT,
      .font_type = FONT_ST,
      .text_enc = FONT_ENC_UTF8,
  });
}

/**
 * @brief  黄闪 10 秒自动关闭到期回调（由心跳计时任务调用）。
 * @note   黄闪计时递减到 0 → 关闭黄闪。
 *         2026-08-17：唯一调用方（心跳计时任务）已 #if 0 停用，黄闪不再
 *         10 秒自动关闭（须 0A 39 显式关闭）；本函数保留待恢复。
 */
void sc_etc_hs_timeout(void) { dev_io_flash_light(false); }

/**
 * @brief  执行 ETC 协议命令。
 * @param  ch   当前通道。
 * @param  cmd  解析后的命令结构体。
 * @note   收到即回：正常 0A 00 0D / 超长 0A 01 0D / 帧错 0A 02 0D；
 *         心跳帧为单向保活，按文档不回。
 */
void sc_etc_execute_cmd(channel_t *ch, const sc_etc_parsed_cmd_t *cmd) {
  if (!cmd)
    return;

  if (cmd->sta != SC_ETC_PARSE_OK) {
    /* 解析失败仍应答：超长 01，帧错/参数错 02 */
    _sc_etc_send_ack(ch, (cmd->sta == SC_ETC_PARSE_ERR_TOOLONG) ? 0x01 : 0x02);
    return;
  }

  switch (cmd->cmd) {
  case SC_ETC_PCMD_DISPLAY:
    _sc_etc_exec_display(&cmd->p.display);
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_LIGHT_RED:
    s_etc_color = COLOR_RED; /* 红灯 → 后续显示红色 */
    dev_io_lane_light(false);
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_LIGHT_GREEN:
    s_etc_color = COLOR_GREEN; /* 绿灯 → 后续显示绿色 */
    dev_io_lane_light(true);
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_LIGHT_YELLOW_ON:
    dev_io_flash_light(true);
    sc_etc_hs_timer_arm(); /* 心跳任务已停用：10 秒自动关闭不再触发，需 0A 39
                              显式关闭 */
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_LIGHT_YELLOW_OFF:
    dev_io_flash_light(false);
    sc_etc_hs_timer_cancel(); /* 心跳任务已停用：计时清零仅记录状态（见
                                 app_sc_etc_proto.c 注释） */
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_BRIGHTNESS:
    _sc_etc_exec_brightness(cmd->p.brightness.level);
    _sc_etc_send_ack(ch, 0x00);
    break;
  case SC_ETC_PCMD_HEARTBEAT:
    /* 心跳帧（0A 50 0D）：解析保留（无害）——仅被识别后丢弃，不回。
     * 2026-08-17：心跳超时显示功能已停用（sc_etc_heartbeat_task #if 0），
     * 本帧不再驱动任何计时。 */
    break;
  default:
    break;
  }
}