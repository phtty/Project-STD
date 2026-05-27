/**
 * @file    dev_uart_channel.h
 * @brief   通用 UART 通道抽象 — 收敛 RS485/RS232 的共有逻辑
 *
 * uart_channel_t 是 channel_t 子类，封装 UART 收发、channel 生命周期、
 * ISR 回调→任务分发 的完整流程。板级差异（UART 实例、RE 引脚、DMA 缓冲区）
 * 在各自 dev_rs485.c / dev_rs232.c 中通过 init 参数传入。
 */

#pragma once

#include <stdint.h>

#include "cmsis_os2.h"
#include "dispatch_types.h"
#include "pl_uart.h"

/** @brief UART 通道实例（channel_t 子类） */
typedef struct {
    channel_t ch;
    pl_uart_handle_t uart;
    osMessageQueueId_t rx_queue;
    bool rs485_mode;      /**< true=RS485 半双工（发送时自动 RE 控制） */
    uint8_t *rx_buf;      /**< DMA 空闲中断接收缓冲区 */
    uint16_t rx_buf_size; /**< 缓冲区大小 */
} uart_channel_t;

extern const ch_ops_t uart_ch_ops;

/**
 * @brief 初始化 UART 通道硬件参数（hw_initcall 阶段，不调 RTOS API）
 *
 * RTOS 资源（rx_queue、app_channel_register）由 uart_channel_task 在启动时创建。
 *
 * @param uart        pl_uart 句柄（pl_uart_get_handle(PL_UART1/3)）
 * @param ch_id       通道 ID（CH_ID_RS485 / CH_ID_RS232）
 * @param rs485_mode  true=RS485 模式（发送时自动 RE 控制）
 * @param rx_buf      DMA 接收缓冲区
 * @param rx_buf_size 缓冲区大小
 */
void uart_channel_init(uart_channel_t *self, pl_uart_handle_t uart,
                       uint8_t ch_id, bool rs485_mode,
                       uint8_t *rx_buf, uint16_t rx_buf_size);
void uart_channel_deinit(uart_channel_t *self);

/** @brief RX 数据到达指示（ISR 上下文，由内部适配回调调用） */
void uart_channel_rx_indicate(uart_channel_t *self, uint16_t len);

/** @brief 注册 ISR 回调（替代板级文件中的 pl_uart_set_rx_cb） */
void uart_channel_register_isr(uart_channel_t *self, pl_uart_handle_t uart);

/** @brief 通用 UART 通道任务入口 — 创建 RTOS 资源 + start_rx + 分发循环 */
void uart_channel_task(void *argument);
