/**
 * @file    pl_uart.h
 * @brief   UART 平台层抽象接口（DMA 空闲中断模式）
 *
 * 封装 STM32F4 USART1 (RS485) 和 USART3 (RS232)，
 * 对上层暴露不透明句柄 pl_uart_handle_t，隐藏 HAL 类型。
 * UART 中断 ISR 全部内聚于 pl_uart.c，不向外暴露。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** @brief UART 实例 ID
 *
 *  **跨板稳定**：板子没有的那路在 g_pl_uart_board[] 里留空，枚举值不变，
 *  这样按名字引用某一路的共享代码不需要条件编译。 */
enum {
    PL_UART1 = 0, /**< USART1 (RS485) */
    PL_UART3,     /**< USART3 (RS232-0) */
    PL_UART6,     /**< USART6 (RS232-1) */
    PL_UART_MAX,
};

/** @brief UART 不透明句柄 */
typedef void *pl_uart_handle_t;

/** @brief 板级 UART 表项（由 boards/<板>/Src/pl_uart_board.c 提供）
 *
 *  PL_UART_MAX 是 A/B 两块板枚举的并集；某块板没有的那几路 .init/.huart 留 NULL，
 *  pl_uart_init 会跳过，pl_uart_get_handle 返回的 ctx 里 huart 为 NULL，
 *  各 API 已经判空返回 -1。 */
typedef struct {
    void (*init)(void); /**< MX_USARTx_UART_Init，NULL 表示本板无此路 */
    void *huart;        /**< &huartx，NULL 表示本板无此路 */
    void *dma_rx;       /**< &hdma_usartx_rx，无 DMA 接收则 NULL */
    uint8_t irq;        /**< USARTx_IRQn，0 表示无 */
    uint8_t dma_irq;    /**< DMAn_Streamm_IRQn，0 表示无 */
} pl_uart_board_entry_t;

extern const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX];

/** @brief 供板级 ISR 调用的中断入口
 *
 *  ISR 向量名（USART1_IRQHandler / DMA2_Stream2_IRQHandler …）必须写在某个 .c 里，
 *  而"本板有哪些中断、哪个 DMA 流属于哪一路"是板级事实，所以向量放在
 *  boards/<板>/Src/pl_uart_board.c，函数体复用这两个共享入口。 */
void pl_uart_irq_handler(uint8_t id);
void pl_uart_dma_irq_handler(uint8_t id);

/** @brief DMA 空闲中断接收回调（ISR 上下文，应尽快返回） */
typedef void (*pl_uart_rx_cb_t)(uint8_t *data, uint16_t len, void *ctx);

/** @brief 收发方向控制回调（Device 层注入，用于 RS485 RE 引脚控制） */
typedef void (*pl_uart_dir_fn_t)(bool tx);

void pl_uart_init(void);
pl_uart_handle_t pl_uart_get_handle(uint8_t id);

/** @brief 阻塞发送，方向回调由 pl_uart_set_dir_cb 注入 */
int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** @brief 注册 DMA 空闲中断接收回调 */
void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx);

/** @brief 注册收发方向控制回调（仅 RS485 需要，RS232 传 NULL） */
void pl_uart_set_dir_cb(pl_uart_handle_t h, pl_uart_dir_fn_t cb);

/** @brief 启动 DMA 空闲中断接收，成功返回 0 */
int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len);
