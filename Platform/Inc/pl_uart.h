/**
 * @file    pl_uart.h
 * @brief   UART 平台层抽象接口（DMA 乒乓双缓冲 circular 模式）
 *
 * 封装 STM32F4 USART1 (RS485) 和 USART3 (RS232)，对上层暴露不透明句柄
 * pl_uart_handle_t，隐藏 HAL 类型。UART 中断 ISR 全部内聚于 pl_uart.c。
 *
 * RX 架构（乒乓双缓冲）:
 *   - 缓冲 2 × block（总长 len = pl_uart_start_rx 传入），DMA circular 自动回绕；
 *   - HT（半完成）冲刷前半块，TC（全完成）冲刷后半块，IDLE 冲刷块内残余字节；
 *   - ISR 经 rx_cb(data, len, ctx) 通知上层：data 指向新字节段起点，len 为段长
 *     （1..block，跨块边界由 HT/TC 中断保证不越界）；
 *   - 上层任务须在下一块被覆盖前（一个 circular 周期内）把数据拷走。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/** @brief UART 实例 ID */
enum {
    PL_UART1 = 0, /**< USART1 (RS485) */
    PL_UART3,     /**< USART3 (RS232-0) */
    PL_UART6,     /**< USART6 (RS232-1) — Project_STD 独有 */
    PL_UART_MAX,
};

/** @brief UART 不透明句柄 */
typedef void *pl_uart_handle_t;

/** @brief DMA 接收回调（ISR 上下文，应尽快返回）：data=段起点，len=段长 */
typedef void (*pl_uart_rx_cb_t)(uint8_t *data, uint16_t len, void *ctx);

/** @brief 收发方向控制回调（Device 层注入，用于 RS485 RE 引脚控制） */
typedef void (*pl_uart_dir_fn_t)(bool tx);

/** @brief RX 乒乓双缓冲通知消息：块号 + 块内偏移 + 长度
 *  由上层 ISR 回调根据 data 指针换算后经队列传给通道任务；
 *  通道任务从 rx_buf + block*block_size + offset 拷贝 len 字节入 RB。 */
typedef struct {
    uint8_t  block;  /**< 块号 0/1（每块 = 缓冲总长的一半） */
    uint8_t  pad;    /**< 对齐填充 */
    uint16_t offset; /**< 块内偏移（本段首字节） */
    uint16_t len;    /**< 本段长度（1..block_size） */
} pl_uart_rx_msg_t;

void pl_uart_init(void);
pl_uart_handle_t pl_uart_get_handle(uint8_t id);

/** @brief 阻塞发送，方向回调由 pl_uart_set_dir_cb 注入 */
int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** @brief 注册 DMA 接收回调 */
void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx);

/** @brief 注册收发方向控制回调（仅 RS485 需要，RS232 传 NULL） */
void pl_uart_set_dir_cb(pl_uart_handle_t h, pl_uart_dir_fn_t cb);

/** @brief 启动 DMA 乒乓双缓冲接收（circular 模式），成功返回 0
 *  @param h    UART 句柄
 *  @param buf 双块缓冲基址（.bss 静态，2 × block 字节）
 *  @param len 缓冲总长 = 2 × block（HT 边界 = len/2） */
int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len);

/** @brief 运行态切换波特率：停 DMA RX → DeInit → 改波特率 → Init → 重挂 RX
 *  @param h    UART 句柄
 *  @param baud 目标波特率
 *  @return 0=成功；-1=参数非法或硬件配置失败
 *  @note  DMA 接收已启动时内部自动停/重挂（circular）；未启动时仅重配外设。
 *         DeInit/Init 会经 MspDeInit/MspInit 重配 GPIO/DMA/NVIC（RS485 RE 方向
 *         控制由 Device 层 dev_rs485 独立管理，不受影响）。 */
int32_t pl_uart_set_baud(pl_uart_handle_t h, uint32_t baud);