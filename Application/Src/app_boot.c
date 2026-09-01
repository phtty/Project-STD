/**
 * @file    app_boot.c
 * @brief   系统启动编排器
 *
 * 职责：RTOS 生命周期 + 硬件无关的模块初始化 + HalfSecTask
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_board_net_cfg.h"
#include "app_boot.h"
#include "app_default_display.h"
#include "app_key.h"
#include "app_net_boot.h"
#include "app_render.h"
#include "app_rs232.h"
#include "app_rs485.h"
#include "app_tcp_client.h"
#include "app_tcp_server.h"
#include "app_test.h"
#include "app_udp.h"
#include "cmsis_os.h"
#include "dev_display.h"
#include "dev_eth.h"
#include "initcall.h"
#include "pl_dwt.h"
#include "pl_gpio.h"
#include "pl_iwdg.h"
#include "pl_rtc.h"

#define SPLASH_DURATION_MS 5000U

static void init_task(void *argument);

/* ---- HalfSecTask: 500ms 喂狗 / 60s RTC 备份 / LED 翻转 ---- */
static void half_sec_task(void *argument) {
  (void)argument;
  uint32_t run_time = 0;
  bool led_state = false;

  for (;;) {
    pl_iwdg_refresh(pl_iwdg_get_handle());

    if (run_time >= 60)
      pl_rtc_bkup_write(pl_rtc_get_handle(), 1, 0);
    else
      run_time++;

    led_state = !led_state;
    pl_gpio_write(PL_PORT_D, 9, led_state); /* LED = PD9 */

    osDelay(500);
  }
}

/* ---- 启动入口 ---- */
void app_boot(void) {
  const osThreadAttr_t attr = {
      .name = "init_task",
      .stack_size = 512 * 4,
      .priority = osPriorityHigh,
  };

  osKernelInitialize();
  osThreadNew(init_task, NULL, &attr);
  osKernelStart();
}

[[maybe_unused]] static void app_splash_display(void) {
  dev_display_t *dev = dev_display_get();
  char fw_line[32];
  char md_line[32];
  char splash_text[96];

  snprintf(fw_line, sizeof(fw_line), "FW:%s", PROGRAM_CODE);
  snprintf(md_line, sizeof(md_line), "MD:%s", dev->module_code);
  snprintf(splash_text, sizeof(splash_text), "%s\n%s", fw_line, md_line);

  dev_display_fill(dev, 0, 0, dev->screen_rows, dev->screen_cols, COLOR_BLACK);
  dev->dirty = false;

  app_render(&(render_cfg_t){
      .type = RENDER_TEXT,
      .x = 0,
      .y = 0,
      .w = dev->screen_rows,
      .h = dev->screen_cols,
      .style =
          &(render_style_t){
              .h_align = ALIGN_CENTER,
              .v_align = ALIGN_CENTER,
              .word_wrap = false,
          },
      .color = COLOR_GREEN,
      .text = splash_text,
      .len = strlen(splash_text),
      .font_size = FONT_SELF_ADAPT,
      .font_type = FONT_ST,
      .text_enc = FONT_ENC_UTF8,
  });

  osDelay(SPLASH_DURATION_MS);
  dev_display_fill(dev, 0, 0, dev->screen_rows, dev->screen_cols, COLOR_BLACK);
  dev->dirty = false;
}

static void init_task(void *argument) {
  (void)argument;

  dev_eth_start();
  sw_board_init(); /* sw_initcall 自注册：协议 + 通道任务 */

  /* 版本落库：PROGRAM_CODE 写入 Sector1 app_info.version，供 IAP 0x03 上报。
   * 16KB 扇区擦除耗时数百 ms，写入前喂一次狗；同值跳过保证每次版本发布只擦一次 */
  pl_iwdg_refresh(pl_iwdg_get_handle());
  int32_t ver_ret = app_board_net_cfg_fw_version_update(PROGRAM_CODE);
  (void)ver_ret; /* 失败不阻断启动（升级中间态/擦写错误静默降级，IAP 0x03 报旧值） */

  /* 网络配置横切应用（app_net_boot 中立模块）：Sector1 net_cfg → netif + TCP
   * Server 口（PROTO_CHONGQING 只应用 netif）。顺序约束：fw_version_update 对
   * 空/损坏扇区初始化时 net_cfg 置 0 → 本函数判无效写默认（accept_write）；
   * ldi_module_init（sw_board_init 内）的 W25 自愈/回写已先于本调用完成。 */
  app_net_boot_apply();

  app_splash_display();

  /* 半秒周期任务 */
  const osThreadAttr_t hst_attr = {
      .name = "half_sec_task",
      .stack_size = 128 * 4,
      .priority = osPriorityLow,
  };
  osThreadNew(half_sec_task, NULL, &hst_attr);

  app_tcp_server_start();
  app_tcp_client_start();

  app_udp_start();
  /* CQ 业务口 UDP（端口 Sector1 net_cfg.udp_port，默认 20103，PROTO_CHONGQING 读） */
  app_udp_cq_start();
  app_rs485_start();
  /* TX1/RX1：地区协议 RS232（USART3） */
  app_rs232_start();
  /* USART6：语音板 TTS 旁路 TX（dev_rs232_voice），禁止协议 bind；不启 RX
   * 通道任务 */
  app_rs232_1_start();

  // app_test_run();
  app_default_display();
  osThreadExit();
}

/* ---- FreeRTOS 钩子 ---- */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  /* 调试时在此设断点，查看 pcTaskName 确定溢出任务 */
  volatile char *name = pcTaskName;
  volatile TaskHandle_t task = xTask;
  (void)name;
  (void)task;
  for (;;)
    ;
}

void vApplicationMallocFailedHook(void) {
  taskDISABLE_INTERRUPTS();
  for (;;)
    ;
}

void vConfigureTimerForRunTimeStats(void) { pl_dwt_init(); }

uint32_t ulGetRunTimeCounterValue(void) { return pl_dwt_get_cycles(); }
