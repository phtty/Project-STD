/**
 * @file    app_uart_baud.h
 * @brief   DIP1 波特率选择与运行态波特率切换（RS232 + RS485 同步）
 *
 * DIP1（PE7，active_low）：ON=115200、OFF=9600，同时作用于 RS232（USART3）与
 * RS485（USART1）两路 UART。上电由 app_uart_baud_init（sw_app_initcall）在通道
 * 任务启动（app_rs232_start / app_rs485_start）之前应用；
 * 运行态亦可被 MTC 7B 40 修改波特率命令调用 app_uart_baud_apply 切换。
 */
#pragma once

#include <stdint.h>

#define APP_UART_BAUD_OFF (9600U)  /**< DIP1 OFF 波特率 */
#define APP_UART_BAUD_ON  (115200U)/**< DIP1 ON 波特率 */

/**
 * @brief  获取当前生效的波特率。
 * @return 当前波特率（9600 / 115200）。
 */
uint32_t app_uart_baud_get(void);

/**
 * @brief  同步切换 RS232 与 RS485 两路 UART 波特率。
 * @param  baud  目标波特率（仅支持 9600 / 115200）。
 * @return 0=成功；-1=参数非法或任一路切换失败。
 * @note   DMA 空闲接收已启动时（运行态切换）内部会先停 DMA、DeInit/Init 后重挂 RX。
 */
int32_t app_uart_baud_apply(uint32_t baud);