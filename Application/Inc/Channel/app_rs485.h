/**
 * @file    app_rs485.h
 * @brief   RS485 通道 Application 层（USART1, RE=PA8）
 *
 * 容器约定：控制块由本模块静态持有，base 是第一个成员（偏移 0，container_of
 * 零开销还原）；协议侧只保存 app_rs485_ccb() 返回的 ccb_t*，断线只改 base.state，
 * 控制块本身不销毁，故该指针永不悬空。
 */
#pragma once

#include "cmsis_os2.h"
#include "app_dispatch.h"

osThreadId_t app_rs485_start(void);

/** @brief 暴露本通道控制块（协议绑定时使用）*/
ccb_t *app_rs485_ccb(void);
