/**
 * @file    app_rs232.c
 * @brief   RS232 通道（USART3=RS232-0 协议通道, USART6=RS232-1 语音专用 TX）
 *
 * RS232-0（USART3）：完整 RX+TX 协议通道，参与多协议调度。
 * RS232-1（USART6）：仅提供 TX 能力给语音板 TTS，**不注册为协议通道**，
 *   不调用 app_channel_dispatch / app_channel_register。
 *
 * RX 路径：pl_uart circular 乒乓（2×640B）经 HT/TC/IDLE 中断增量通知，
 * ISR 回调把「块号 + 偏移 + 长度」消息放入 rx_queue（深度 2），
 * 任务按消息从对应块拷贝到 RB（app_channel_dispatch）。
 *
 * @note   架构约束（doc/05_…/01_architecture.md §2.3 / §5）：
 *   PL_UART6 / CH_ID_RS232_1 仅用于语音板 TTS 发送；
 *   地区协议禁止绑定 CH_ID_RS232_1。
 */

#include "app_rs232.h"

#include "FreeRTOS.h"
#include "app_dispatch.h"
#include "dev_rs232.h"
#include "pl_uart.h"

typedef struct {
  channel_t me;
  pl_uart_handle_t uart;
  osMessageQueueId_t rx_queue;
  uint8_t *rx_buf;
  uint16_t rx_buf_size;   /* 总长 = 2 × block（circular 周期） */
  uint16_t rx_block_size; /* 半块 = RS232_BUF_SIZE */
} rs232_ch_t;

/* ---- RS232-0 (USART3) RX queue 静态分配（深度 2：乒乓双块各一消息在途） ---- */
static StaticQueue_t s_rs232_0_rx_cb;
static pl_uart_rx_msg_t s_rs232_0_rx_buf[2];
static const osMessageQueueAttr_t s_rs232_0_rx_attr = {
    .name = "rs232_0_rx",
    .cb_mem = &s_rs232_0_rx_cb,
    .cb_size = sizeof(s_rs232_0_rx_cb),
    .mq_mem = s_rs232_0_rx_buf,
    .mq_size = sizeof(s_rs232_0_rx_buf),
};

/* ---- RS232 task attr ---- */
static const osThreadAttr_t s_rs232_0_attr = {
    .name = "rs232_0_task",
    .stack_size = 256 * 4,
    .priority = osPriorityNormal,
};

/* ---- RS232-0 实例（协议通道） ---- */
static rs232_ch_t g_rs232_0 = {.me = {.ch_id = CH_ID_RS232}};

/* ---- ops（仅 RS232-0 使用） ---- */
static int32_t rs232_send(channel_t *ch, const uint8_t *data, uint16_t len) {
  rs232_ch_t *self = container_of(ch, rs232_ch_t, me);
  return pl_uart_send(self->uart, data, len, 100);
}

static const ch_ops_t rs232_ops = {.send = rs232_send};

/* ---- ISR → 任务通知 ---- */
static void rs232_isr_cb(uint8_t *data, uint16_t len, void *ctx) {
  rs232_ch_t *self = (rs232_ch_t *)ctx;
  uint32_t off      = (uint32_t)(data - self->rx_buf);

  /* 段指针换算为「块号 + 块内偏移」；len 由 pl_uart 保证 ≤ block */
  pl_uart_rx_msg_t m = {
      .block  = (uint8_t)(off / self->rx_block_size),
      .offset = (uint16_t)(off % self->rx_block_size),
      .len    = len,
  };
  osMessageQueuePut(self->rx_queue, &m, 0, 0);
}

/* ---- RS232-0 任务循环（完整协议通道） ---- */
static void rs232_task(void *argument) {
  rs232_ch_t *self = (rs232_ch_t *)argument;

  self->rx_queue = osMessageQueueNew(
      2, sizeof(pl_uart_rx_msg_t), &s_rs232_0_rx_attr);
  if (self->rx_queue == NULL) {
    osThreadExit();
    return;
  }
  app_channel_register(self->me.ch_id, &self->me);

  pl_uart_set_rx_cb(self->uart, rs232_isr_cb, self);
  pl_uart_start_rx(self->uart, self->rx_buf, self->rx_buf_size);

  for (;;) {
    pl_uart_rx_msg_t m;
    if (osMessageQueueGet(self->rx_queue, &m, 0, osWaitForever) == osOK) {
      /* 按块号从对应缓冲拷贝 len 字节入调度框架 */
      uint8_t *src = self->rx_buf + (uint16_t)m.block * self->rx_block_size + m.offset;
      app_channel_dispatch(&self->me, src, m.len);
    }
  }
}

/* ---- 公开 API ---- */

/**
 * @brief  启动 RS232-0 协议通道（USART3, CH_ID_RS232）
 * @return 通道任务句柄
 */
osThreadId_t app_rs232_start(void) {
  rs232_ch_t *self = &g_rs232_0;
  self->me.ops = &rs232_ops;
  self->uart = pl_uart_get_handle(PL_UART3);
  self->rx_buf = dev_rs232_get_buf(0);
  self->rx_block_size = RS232_BUF_SIZE;
  self->rx_buf_size   = 2U * RS232_BUF_SIZE; /* 乒乓双块：640→1280B */
  return osThreadNew(rs232_task, self, &s_rs232_0_attr);
}
 
/**
 * @brief  RS232-1 启动桩（USART6 语音板专用，纯 TX）
 *
 * @note   按架构约束（doc/05_…/01_architecture.md §2.3 / §5），
 *   USART6 / CH_ID_RS232_1 仅用于语音板 TTS 发送（PL_UART6），
 *   不注册为协议通道，不创建 RX 任务，不参与多协议调度。
 *   语音 TX 由 dev_rs232_voice 层直接调用 pl_uart_send(PL_UART6) 完成。
 *
 * @return 始终返回 nullptr（无通道任务）
 */
osThreadId_t app_rs232_1_start(void) {
  /* 语音板 TX 路径：dev_rs232_voice → pl_uart_send(PL_UART6)，
   * 不经过 dispatch 框架，无需创建通道任务或注册 CH_ID_RS232_1。 */
  return nullptr;
}