/**
 * @file    app_rs232.h
 * @brief   RS232 通道 Application 层（双路）
 *
 * 真实映射（以 pl_uart 句柄为准）：RS232-0 = USART3 (PL_UART3, PB10/PB11)，
 * RS232-1 = USART6 (PL_UART6, PC6/PC7)。RS485 走另一颗 USART1，与本模块无关。
 *
 * 粒度：一个 ccb = 一个物理端点，两路各自导出控制块（"rs232_0"/"rs232_1"），
 * 与 RS485 对齐 —— 走哪路 UART 是对象身份，不是发送参数；合成一个 ccb 会把
 * 选择逻辑塞进 ops->send 内部，等于把通道枚举换个地方藏起来。
 *
 * 容器约定：两个控制块由本模块静态持有，base 是第一个成员（偏移 0，container_of
 * 零开销还原）；协议侧只保存 app_rs232_0_ccb()/app_rs232_1_ccb() 返回的 app_ccb_t*，
 * 断线只改 base.state，控制块本身不销毁，故该指针永不悬空。
 */
#pragma once

#include "cmsis_os2.h"
#include "app_dispatch.h"

/** @brief 启动 RS232-0（USART3）通道任务
 *  @return 任务句柄，创建失败返回 NULL */
osThreadId_t app_rs232_start(void);

/** @brief 启动 RS232-1（USART6）通道任务
 *  @return 任务句柄，创建失败返回 NULL */
osThreadId_t app_rs232_1_start(void);

/** @brief 暴露 RS232-0（USART3）控制块（协议绑定时使用）*/
app_ccb_t *app_rs232_0_ccb(void);

/** @brief 暴露 RS232-1（USART6）控制块（协议绑定时使用）*/
app_ccb_t *app_rs232_1_ccb(void);
