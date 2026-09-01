/**
 * @file    app_cq_proto_cmd.c
 * @brief   重庆高速二代费显协议（CQ）命令执行
 *
 * 执行层直接调用 STD 设备接口（doc/08 惯例，无服务层）：
 *   - 文本/全屏/位图 → app_render + dev_display_commit_frame
 *   - 亮度 → 光敏任务挂起/恢复 + dev_display_set_brightness（对齐贵州 '8'
 * 命令方式）
 *   - 语音 → dev_rs232_voice 旁路 USART6（语音板 FD 帧，'|' 组合符原样透传）
 *   - 黄闪 → dev_io_flash_light（倒计时在 timer_task 递减）
 *   - setip → app_board_net_cfg_update 落盘 Sector1 + NVIC_SystemReset 重启生效
 *   - 二进制重启/搜索应答 → channel_send 回源 / app_udp_broadcast（10011）
 * JSON 命令无应答帧。
 */

#include "app_cq_proto_cmd.h"

#include <string.h>

#include "app_board_net_cfg.h"
#include "app_light_sensor.h"
#include "app_render.h"
#include "app_udp.h"
#include "cmsis_os2.h"
#include "crc_utils.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"
#include "stm32f4xx_hal.h"

/* ---- 32×32 通行灯位图（每行 4B MSB-first，与 RENDER_BITMAP 格式一致；共
 * 128B）---- */

/* 绿箭（turn="g"） */
static const uint8_t s_cq_bmp_green_arrow[128] = {
    0x10, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00,
    0xFE, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x08,
    0x1F, 0xC0, 0x00, 0x0C, 0x0F, 0xE0, 0x00, 0x0E, 0x07, 0xF0, 0x00, 0x0F,
    0x03, 0xF8, 0x00, 0x0F, 0x01, 0xFC, 0x00, 0x0F, 0x00, 0xFE, 0x00, 0x0F,
    0x00, 0x7F, 0x00, 0x0F, 0x00, 0x3F, 0x80, 0x0F, 0x00, 0x1F, 0xC0, 0x0F,
    0x00, 0x0F, 0xE0, 0x0F, 0x00, 0x07, 0xF0, 0x0F, 0x00, 0x03, 0xF8, 0x0F,
    0x00, 0x01, 0xFC, 0x0F, 0x00, 0x00, 0xFE, 0x0F, 0x00, 0x00, 0x7C, 0x0F,
    0x00, 0x00, 0x38, 0x0F, 0x00, 0x00, 0x10, 0x0F, 0x00, 0x00, 0x00, 0x0F,
    0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x0F,
    0x00, 0x00, 0x00, 0x0F, 0x07, 0xFF, 0xFF, 0xFF, 0x03, 0xFF, 0xFF, 0xFF,
    0x01, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
};

/* 红叉（turn="r"） */
static const uint8_t s_cq_bmp_red_cross[128] = {
    0x10, 0x00, 0x00, 0x08, 0x38, 0x00, 0x00, 0x1C, 0x7C, 0x00, 0x00, 0x3E,
    0xFE, 0x00, 0x00, 0x7F, 0x7F, 0x00, 0x00, 0xFE, 0x3F, 0x80, 0x01, 0xFC,
    0x1F, 0xC0, 0x03, 0xF8, 0x0F, 0xE0, 0x07, 0xF0, 0x07, 0xF0, 0x0F, 0xE0,
    0x03, 0xF8, 0x1F, 0xC0, 0x01, 0xFC, 0x3F, 0x80, 0x00, 0xFE, 0x7F, 0x00,
    0x00, 0x7F, 0xFE, 0x00, 0x00, 0x3F, 0xFC, 0x00, 0x00, 0x1F, 0xF8, 0x00,
    0x00, 0x0F, 0xF0, 0x00, 0x00, 0x0F, 0xF0, 0x00, 0x00, 0x1F, 0xF8, 0x00,
    0x00, 0x3F, 0xFC, 0x00, 0x00, 0x7F, 0xFE, 0x00, 0x00, 0xFE, 0x7F, 0x00,
    0x01, 0xFC, 0x3F, 0x80, 0x03, 0xF8, 0x1F, 0xC0, 0x07, 0xF0, 0x0F, 0xE0,
    0x0F, 0xE0, 0x07, 0xF0, 0x1F, 0xC0, 0x03, 0xF8, 0x3F, 0x80, 0x01, 0xFC,
    0x7F, 0x00, 0x00, 0xFE, 0xFE, 0x00, 0x00, 0x7F, 0x7C, 0x00, 0x00, 0x3E,
    0x38, 0x00, 0x00, 0x1C, 0x10, 0x00, 0x00, 0x08,
};

/* ---- 12B 二进制应答帧（CRC16-XMODEM 大端，值按协议文档 2.0.1 固定）---- */
static const uint8_t s_cq_bin_reboot_rsp[12] = {
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xA7, 0x31, 0xF4, 0x36};

/* ---- screen 命令记录（仅记录，不写 Flash、不切模组）---- */
static int s_cq_screen_led = -1;

/* ---- 心跳故障屏文案 ---- */
static const char s_cq_fault_line0[] = "车道关闭";
static const char s_cq_fault_line2[] = "请择道行驶!";

typedef void (*cq_cmd_fn_t)(channel_t *ch, const cq_parsed_cmd_t *cmd);

/* ---- 命令处理函数前向声明（命令分发表定义于实现之前）---- */
static void _cq_exec_text1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_tra1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_pic1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_light1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_voice_ctrl1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_voice1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_voice_play1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_warn1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_syn1(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_setip(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_screen(channel_t *ch, const cq_parsed_cmd_t *cmd);
static void _cq_exec_full(channel_t *ch, const cq_parsed_cmd_t *cmd);

/* ---- 命令分发表（JSON 命令名 → 处理函数）---- */
static const struct {
  cq_pcmd_t cmd;
  cq_cmd_fn_t fn;
} g_cq_cmd_table[] = {
    {CQ_PCMD_TEXT1, _cq_exec_text1},
    {CQ_PCMD_TRA1, _cq_exec_tra1},
    {CQ_PCMD_PIC1, _cq_exec_pic1},
    {CQ_PCMD_LIGHT1, _cq_exec_light1},
    {CQ_PCMD_VOICE_CONTROL1, _cq_exec_voice_ctrl1},
    {CQ_PCMD_VOICE1, _cq_exec_voice1},
    {CQ_PCMD_VOICE_PLAY1, _cq_exec_voice_play1},
    {CQ_PCMD_WARN1, _cq_exec_warn1},
    {CQ_PCMD_SYN1, _cq_exec_syn1},
    {CQ_PCMD_SETIP, _cq_exec_setip},
    {CQ_PCMD_SCREEN, _cq_exec_screen},
    {CQ_PCMD_FULL, _cq_exec_full},
};

/**
 * @brief  协议颜色 0红/1绿/2黄 → 显示颜色；越界回退绿。
 */
static display_color_t _cq_map_color(uint8_t idx) {
  switch (idx) {
  case 0:
    return COLOR_RED;
  case 1:
    return COLOR_GREEN;
  case 2:
    return COLOR_YELLOW;
  default:
    return COLOR_GREEN;
  }
}

/**
 * @brief  渲染单行 GB2312 文本：先清该行矩形（RENDER_FILL 黑），
 *         再按像素宽截断（ASCII=字号/2 px、GBK=字号 px）到屏宽渲染。
 * @param  text   GB2312 字节文本。
 * @param  len    文本字节数。
 * @param  y      行起点 y。
 * @param  font   字号像素（16/24/32）。
 * @param  color  显示颜色。
 */
static void _cq_render_gbk_line(const char *text, uint16_t len, uint16_t y,
                                uint16_t font, display_color_t color) {
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;

  /* 先清该行（矩形黑色填充） */
  app_render(&(render_cfg_t){
      .type = RENDER_FILL,
      .x = 0,
      .y = y,
      .w = d->screen_rows,
      .h = font,
      .color = COLOR_BLACK,
  });

  if (text == NULL || len == 0U)
    return;

  /* 字符宽度截断：按像素宽累计截断到屏宽 */
  uint16_t px = 0;
  uint16_t i = 0;
  while (i < len) {
    uint8_t b = (uint8_t)text[i];
    bool gbk = (b >= 0x81U && b <= 0xFEU);
    if (gbk && i + 1U >= len)
      break; /* 半个 GBK 尾字节：不渲染残缺字符 */
    uint16_t w = gbk ? font : (uint16_t)(font / 2U);
    if ((uint32_t)px + w > d->screen_rows)
      break;
    px += w;
    i += gbk ? 2U : 1U;
  }
  if (i == 0)
    return;

  app_render(&(render_cfg_t){
      .type = RENDER_TEXT,
      .x = 0,
      .y = y,
      .w = d->screen_rows,
      .h = font,
      .style =
          &(render_style_t){
              .h_align = ALIGN_LEFT_UP,
              .v_align = ALIGN_LEFT_UP,
              .word_wrap = false,
          },
      .color = color,
      .text = text,
      .len = i,
      .font_size = (font_size_t)font,
      .font_type = FONT_ST,
      .text_enc = FONT_ENC_GBK,
  });
}

/**
 * @brief  text1：scr==0 先全屏清黑；ln0~ln7 逐行渲染
 *         （行号限制：ln4 字号≤24，ln5~ln7 字号≤16；行超出屏高跳过）。
 */
static void _cq_exec_text1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;

  if (cmd->p.text1.scr_clear) {
    app_render(&(render_cfg_t){
        .type = RENDER_FILL,
        .x = 0,
        .y = 0,
        .w = 0,
        .h = 0,
        .color = COLOR_BLACK,
    });
  }

  for (uint8_t i = 0; i < 8; i++) {
    const cq_line_t *L = &cmd->p.text1.ln[i];
    if (!L->valid)
      continue;
    /* 行号限制：ln4 仅字号≤24；ln5~ln7 仅≤16，超限跳过该行 */
    if (i == 4 && L->font > 24)
      continue;
    if (i >= 5 && L->font > 16)
      continue;
    uint16_t y = (uint16_t)i * L->font;
    if ((uint32_t)y + L->font > d->screen_cols)
      continue; /* 行超出屏高跳过 */
    _cq_render_gbk_line(L->text, L->text_len, y, L->font,
                        _cq_map_color(L->color));
  }

  dev_display_commit_frame(d);
}

/**
 * @brief  tra1：右下角 32×32 红叉 / 绿箭位图（先清该区域再绘制）。
 */
static void _cq_exec_tra1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;
  if (d->screen_rows < 32U || d->screen_cols < 32U)
    return;

  uint16_t x = (uint16_t)(d->screen_rows - 32U);
  uint16_t y = (uint16_t)(d->screen_cols - 32U);

  app_render(&(render_cfg_t){
      .type = RENDER_FILL,
      .x = x,
      .y = y,
      .w = 32,
      .h = 32,
      .color = COLOR_BLACK,
  });
  app_render(&(render_cfg_t){
      .type = RENDER_BITMAP,
      .x = x,
      .y = y,
      .w = 32,
      .h = 32,
      .color = (cmd->p.tra1.turn == 1U) ? COLOR_GREEN : COLOR_RED,
      .bitmap =
          (cmd->p.tra1.turn == 1U) ? s_cq_bmp_green_arrow : s_cq_bmp_red_cross,
  });
  dev_display_commit_frame(d);
}

/** @brief  pic1：stub（解析后忽略）。 */
static void _cq_exec_pic1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  (void)cmd;
}

/**
 * @brief  light1：0 → 恢复光敏自动调光；1~15 → 挂起光敏 + 亮度 level/2+1（上限
 * 8）。
 * @note   对齐贵州 '8' 命令实现方式。
 */
static void _cq_exec_light1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;

  int level = cmd->p.light1.level;
  if (level == 0) {
    if (g_light_sensor_task_handle != nullptr)
      osThreadResume(g_light_sensor_task_handle);
    return;
  }
  if (g_light_sensor_task_handle != nullptr)
    osThreadSuspend(g_light_sensor_task_handle); /* 手动亮度 → 关自动调光 */
  uint8_t b = (uint8_t)(level / 2 + 1);
  if (b > DEV_DISPLAY_BRIGHTNESS_MAX)
    b = DEV_DISPLAY_BRIGHTNESS_MAX;
  dev_display_set_brightness(d, b);
}

/** @brief  voice_control1：level/2+1（上限 7）→ dev_rs232_voice_volume。 */
static void _cq_exec_voice_ctrl1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  int grade = cmd->p.voice_ctrl1.level / 2 + 1;
  if (grade > 7)
    grade = 7;
  dev_rs232_voice_volume((uint8_t)grade);
}

/**
 * @brief  voice1：t 文本（GB2312 字节，保留 '|' 组合符）→
 * dev_rs232_voice_play， 超过接口上限 200B 截断。
 */
static void _cq_exec_voice1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  uint16_t len = cmd->p.voice1.text_len;
  if (len > DEV_RS232_VOICE_MAX_TEXT)
    len = DEV_RS232_VOICE_MAX_TEXT;
  dev_rs232_voice_play((const uint8_t *)cmd->p.voice1.text, len);
}

/** @brief  voice_play1：stub（解析后忽略）。 */
static void _cq_exec_voice_play1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  (void)cmd;
}

/**
 * @brief  warn1：0 关黄闪并清倒计时；>0 开黄闪 + 倒计时（timer_task 每秒递减，
 *         归零关）；-1 常开（协议文档 level 参数未实现，忽略并注释说明）。
 */
static void _cq_exec_warn1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  cq_proto_warn_set(cmd->p.warn1.second);
}

/** @brief  syn1：心跳帧，计数器清零由 handle_task 统一处理，此处无操作。 */
static void _cq_exec_syn1(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  (void)cmd;
}

/**
 * @brief  setip：解析校验（parse 层）→ app_board_net_cfg_update 落盘 Sector1
 *         → NVIC_SystemReset 重启生效（无应答，对齐 LDI 0AH 落盘语义）。
 * @note   CQ 构建不含 LDI，仅写 Sector1；W25 镜像由 LDI
 * 配置模块维护（未编入）。
 */
static void _cq_exec_setip(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;

  /* 方案 B（2026-08-21）：setip 写 CQ UDP 业务口（udp_port=命令下发端口），
   * TCP 业务口 port 保留现值（get 失败用出厂默认 9528）——setip 不再污染 TCP
   * 口。 落盘返回值忽略、无条件复位（写失败重启后回旧配置，与裸机一致， doc/03
   * PartB B.7.1 边界语义）。 */
  app_board_net_cfg_t cur_cfg;
  uint32_t tcp_port =
      (app_board_net_cfg_get(&cur_cfg) == 0) ? cur_cfg.port : 9528U;
  (void)app_board_net_cfg_update(cmd->p.setip.ip, cmd->p.setip.mask,
                                 cmd->p.setip.gw, tcp_port, cmd->p.setip.port);
  NVIC_SystemReset();
}

/** @brief  screen：仅记录到 static 变量，不写
 * Flash、不切模组（本次不含模组排布）。 */
static void _cq_exec_screen(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  s_cq_screen_led = cmd->p.screen.led;
}

/** @brief  读取 screen 命令最近记录的 led 值（-1 = 未收到过）。 */
int cq_screen_led_get(void) { return s_cq_screen_led; }

/**
 * @brief  full：0红/1绿/2黄/3白 全屏 RENDER_FILL。
 * @note   3=白：红绿双基色屏无法物理显示纯白，映射红绿全亮即黄。
 */
static void _cq_exec_full(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  (void)ch;
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;

  display_color_t color;
  switch (cmd->p.full.color) {
  case 0:
    color = COLOR_RED;
    break;
  case 1:
    color = COLOR_GREEN;
    break;
  case 2:
    color = COLOR_YELLOW;
    break;
  case 3:
    color = COLOR_WHITE;
    break;
  default:
    color = COLOR_BLACK;
    break; 
  }
  app_render(&(render_cfg_t){
      .type = RENDER_FILL,
      .x = 0,
      .y = 0,
      .w = 0,
      .h = 0,
      .color = color,
  });
  dev_display_commit_frame(d);
}

/**
 * @brief  12B 二进制重启请求：单播应答 12B 到源通道 → 延时 100ms → 软复位。
 */
static void _cq_exec_bin_reboot(channel_t *ch) {
  channel_send(ch, (uint8_t *)s_cq_bin_reboot_rsp, sizeof(s_cq_bin_reboot_rsp));
  osDelay(100);
  NVIC_SystemReset();
}

/**
 * @brief  12B 二进制搜索请求：广播 25B 应答（ip+mask+gw+port BE+CRC16-XMODEM
 * 大端） 经 10011 搜索口（app_udp_broadcast）。
 * @note   CRC 覆盖应答帧字节 [2..22]（crc16_xmodem：poly 0x8408/init
 * 0xFFFF/取反）。
 */
static void _cq_exec_bin_search(void) {
  app_board_net_cfg_t cfg;
  uint8_t ip[4], mask[4], gw[4];
  uint16_t port;

  if (app_board_net_cfg_get(&cfg) == 0 &&
      (cfg.ip[0] | cfg.ip[1] | cfg.ip[2] | cfg.ip[3]) != 0U) {
    memcpy(ip, cfg.ip, sizeof(ip));
    memcpy(mask, cfg.mask, sizeof(mask));
    memcpy(gw, cfg.gw, sizeof(gw));
  } else {
    /* Sector1 空/损坏/非法 → 出厂默认
     * 192.168.1.5/255.255.255.0/192.168.1.1/20103 */
    static const uint8_t def_ip[4] = {192, 168, 1, 5};
    static const uint8_t def_mask[4] = {255, 255, 255, 0};
    static const uint8_t def_gw[4] = {192, 168, 1, 1};
    memcpy(ip, def_ip, sizeof(ip));
    memcpy(mask, def_mask, sizeof(mask));
    memcpy(gw, def_gw, sizeof(gw));
  }

  /* 端口上报 CQ 实际业务口（app_udp_cq_get_port）而非 Sector1 net_cfg.port：
   * dev 共存构建下 net_cfg.port 属 LDI 语义（出厂 9528），直接上报会引导
   * 上位机连错口；PROTO_CHONGQING 下两者同源（_udp_cq_read_port 读同一记录）。
   */
  port = app_udp_cq_get_port();

  uint8_t buf[25];
  buf[0] = 0xFF;
  buf[1] = 0xFF;
  buf[2] = 0x00;
  buf[3] = 0x00;
  buf[4] = 0x00;
  buf[5] = 0x00;
  buf[6] = 0x00;
  buf[7] = 0x0F;
  buf[8] = 0xA5;
  memcpy(&buf[9], ip, 4);
  memcpy(&buf[13], mask, 4);
  memcpy(&buf[17], gw, 4);
  buf[21] = (uint8_t)(port >> 8); /* 端口高字节在前 */
  buf[22] = (uint8_t)(port & 0xFFU);
  uint16_t crc = crc16_xmodem(&buf[2], 21); /* 字节 [2..22] */
  buf[23] = (uint8_t)(crc >> 8);
  buf[24] = (uint8_t)(crc & 0xFFU);

  app_udp_broadcast(buf, sizeof(buf));
}

/**
 * @brief  渲染心跳超时故障屏：「车道关闭」32pt 红（行0）+「请择道行驶!」（行2）
 *         + 右下角红叉位图；行超出屏高跳过（小屏防御）。
 */
void cq_render_fault_screen(void) {
  dev_display_t *d = dev_display_get();
  if (d == NULL)
    return;

  app_render(&(render_cfg_t){
      .type = RENDER_FILL,
      .x = 0,
      .y = 0,
      .w = 0,
      .h = 0,
      .color = COLOR_BLACK,
  });

  /* 行0：车道关闭（32pt 红居中） */
  if (d->screen_cols >= 32U) {
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT,
        .x = 0,
        .y = 0,
        .w = d->screen_rows,
        .h = 32,
        .style =
            &(render_style_t){
                .h_align = ALIGN_CENTER,
                .v_align = ALIGN_CENTER,
                .word_wrap = false,
            },
        .color = COLOR_RED,
        .text = s_cq_fault_line0,
        .len = sizeof(s_cq_fault_line0) - 1U,
        .font_size = FONT_32,
        .font_type = FONT_ST,
        .text_enc = FONT_ENC_UTF8,
    });
  }

  /* 行2：请择道行驶!（行1 留空，对齐协议文档布局） */
  if (d->screen_cols >= 96U) {
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT,
        .x = 0,
        .y = 64,
        .w = d->screen_rows,
        .h = 32,
        .style =
            &(render_style_t){
                .h_align = ALIGN_CENTER,
                .v_align = ALIGN_CENTER,
                .word_wrap = false,
            },
        .color = COLOR_RED,
        .text = s_cq_fault_line2,
        .len = sizeof(s_cq_fault_line2) - 1U,
        .font_size = FONT_32,
        .font_type = FONT_ST,
        .text_enc = FONT_ENC_UTF8,
    });
  }

  /* 右下角红叉 */
  if (d->screen_rows >= 32U && d->screen_cols >= 32U) {
    uint16_t x = (uint16_t)(d->screen_rows - 32U);
    uint16_t y = (uint16_t)(d->screen_cols - 32U);
    app_render(&(render_cfg_t){
        .type = RENDER_BITMAP,
        .x = x,
        .y = y,
        .w = 32,
        .h = 32,
        .color = COLOR_RED,
        .bitmap = s_cq_bmp_red_cross,
    });
  }

  dev_display_commit_frame(d);
}

/**
 * @brief  CQ 命令分发执行入口。
 */
void cq_execute_cmd(channel_t *ch, const cq_parsed_cmd_t *cmd) {
  if (cmd == NULL || cmd->sta != CQ_PARSE_OK)
    return;

  /* 二进制帧独立处理 */
  if (cmd->cmd == CQ_PCMD_BIN_REBOOT) {
    _cq_exec_bin_reboot(ch);
    return;
  }
  if (cmd->cmd == CQ_PCMD_BIN_SEARCH) {
    _cq_exec_bin_search();
    return;
  }

  /* JSON 命令查表分派 */
  for (size_t i = 0; i < sizeof(g_cq_cmd_table) / sizeof(g_cq_cmd_table[0]);
       i++) {
    if (g_cq_cmd_table[i].cmd == cmd->cmd) {
      g_cq_cmd_table[i].fn(ch, cmd);
      return;
    }
  }
}