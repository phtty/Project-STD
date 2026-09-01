/**
 * @file    app_uart_baud.c
 * @brief   DIP1 波特率选择与运行态波特率切换（RS232 + RS485 同步）
 *
 * 初始化时序（调查结论）：
 *   main.c hw initcall → pl_uart_init（hw_pl lvl1，MX_USARTx_UART_Init @9600）
 *   → dev_key_init（hw_dev lvl2，DIP 表就绪）
 *   → RTOS → init_task → sw_board_init（sw initcall：本模块 sw_app lvl3）
 *   → app_rs485_start / app_rs232_start（此时才 pl_uart_start_rx 挂 DMA RX）
 * 因此本模块在 sw 阶段读取 dev_key_get_state(DEV_KEY_DIP1) 并重配两路波特率，
 * 早于通道任务启动，DMA RX 未挂，切换安全。运行态切换由 pl_uart_set_baud 处理 DMA 重挂。
 */

#include "app_uart_baud.h"

#include "initcall.h"
#include "dev_key.h"
#include "pl_uart.h"

static uint32_t s_app_uart_baud = APP_UART_BAUD_OFF; /**< 当前生效波特率 */

uint32_t app_uart_baud_get(void)
{
    return s_app_uart_baud;
}

int32_t app_uart_baud_apply(uint32_t baud)
{
    if (baud != APP_UART_BAUD_OFF && baud != APP_UART_BAUD_ON)
        return -1;

    pl_uart_handle_t h_rs485 = pl_uart_get_handle(PL_UART1);
    pl_uart_handle_t h_rs232 = pl_uart_get_handle(PL_UART3);
    if (!h_rs485 || !h_rs232)
        return -1;

    /* RS485 与 RS232 同步切换；任一路失败即中止（避免两路口波特率不一致） */
    if (pl_uart_set_baud(h_rs485, baud) != 0)
        return -1;
    if (pl_uart_set_baud(h_rs232, baud) != 0)
        return -1;

    s_app_uart_baud = baud;
    return 0;
}

/**
 * @brief  上电波特率初始化：读 DIP1 并应用到 RS232 + RS485。
 * @note   DIP1（PE7，active_low）：true=ON=115200，false=OFF=9600。
 *         DIP2（PE8）已被 app_render 字库芯片选择占用，不得改动。
 */
static void app_uart_baud_init(void)
{
    uint32_t baud = dev_key_get_state(DEV_KEY_DIP1) ? APP_UART_BAUD_ON : APP_UART_BAUD_OFF;
    (void)app_uart_baud_apply(baud);
}
sw_app_initcall(app_uart_baud_init);