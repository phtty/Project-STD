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
    void *dma_tx;       /**< &hdma_usartx_tx，无 DMA 发送则 NULL（此时只能用轮询模式） */
    uint8_t irq;        /**< USARTx_IRQn，0 表示无 */
    uint8_t dma_irq;    /**< DMAn_Streamm_IRQn（RX），0 表示无 */
    uint8_t dma_tx_irq; /**< DMAn_Streamm_IRQn（TX），0 表示无 */
} pl_uart_board_entry_t;

extern const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX];

/** @brief 供板级 ISR 调用的中断入口
 *
 *  ISR 向量名（USART1_IRQHandler / DMA2_Stream2_IRQHandler …）必须写在某个 .c 里，
 *  而"本板有哪些中断、哪个 DMA 流属于哪一路"是板级事实，所以向量放在
 *  boards/<板>/Src/pl_uart_board.c，函数体复用这两个共享入口。 */
void pl_uart_irq_handler(uint8_t id);
void pl_uart_dma_irq_handler(uint8_t id);
/** @brief TX DMA 流的中断入口（与 RX 分开：两条流各有各的向量，
 *         对没在中断的那条调 HAL_DMA_IRQHandler 是没意义的） */
void pl_uart_dma_tx_irq_handler(uint8_t id);

/** @brief DMA 空闲中断接收回调（ISR 上下文，应尽快返回） */
typedef void (*pl_uart_rx_cb_t)(uint8_t *data, uint16_t len, void *ctx);

/** @brief 收发方向控制回调（Device 层注入，用于 RS485 RE 引脚控制） */
typedef void (*pl_uart_dir_fn_t)(bool tx);

void pl_uart_init(void);
pl_uart_handle_t pl_uart_get_handle(uint8_t id);

/** @brief 发送模式 */
typedef enum {
    PL_UART_TX_POLL = 0, /**< 轮询 TXE，阻塞。短帧、或 RTOS 未就绪的早期路径 */
    PL_UART_TX_DMA,      /**< DMA 搬运，阻塞至 TC（最后一个字节真正发完） */
} pl_uart_tx_mode_t;

/** @brief 发送公共入口 —— **两种模式共用同一把 TX 锁**
 *
 *  为什么必须共锁：两条路径都往同一条半双工总线上写。分开上锁等于没锁 ——
 *  一端连着给多张从卡发帧时，DMA 那次还没发完，轮询那次就把总线抢了。
 *
 *  **timeout 在两种模式下都必须大于本帧的物理发送时间**，只是用途不同：
 *   · POLL：它是 HAL 的轮询预算 —— 不够会在帧中间超时返回，对端收到残帧
 *   · DMA ：它是等 TC 信号量的上限 —— 不够会在还没发完时就中止 DMA
 *  调用方很难正确估算（要知道波特率、10 bit/字节、还要留余量），
 *  故本函数**替他兜一个下限**：传小于该值的会被抬到 物理时间 + 余量。
 *  （原实现把 100ms 写死在 app_rs485.c 里，@115200 只够 1152 字节。）
 *
 *  DMA 模式下 buf **必须 DMA 可达（不能在 CCMRAM）**，本函数会拦下并打印；
 *  另外本板必须给该路配了 TX DMA（g_pl_uart_board[].dma_tx），否则返回 -1。
 *
 *  两模式都在返回前把方向回调切回接收（RS485 的 RE）—— 失败/超时路径也会切，
 *  不会把总线留在发送态。 */
int32_t pl_uart_send_ex(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms,
                        pl_uart_tx_mode_t mode);

/** @brief 轮询发送（短帧、早期路径） */
int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** @brief DMA 发送（长帧：发送期间不占 CPU，任务阻塞在信号量上） */
int32_t pl_uart_send_dma(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** @brief 注册 DMA 空闲中断接收回调 */
void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx);

/** @brief 注册收发方向控制回调（仅 RS485 需要，RS232 传 NULL） */
void pl_uart_set_dir_cb(pl_uart_handle_t h, pl_uart_dir_fn_t cb);

/** @brief 启动 DMA 空闲中断接收，成功返回 0 */
int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len);
