/**
 * @file    pl_exti.h
 * @brief   EXTI 外部中断 Platform 层抽象接口
 */

#pragma once

#include <stdint.h>

/** @brief EXTI 回调（pin=触发引脚, ctx=用户上下文） */
typedef void (*pl_exti_cb_t)(uint16_t pin, void *ctx);

void pl_exti_init(void);
void pl_exti_register_cb(uint16_t pin, pl_exti_cb_t cb, void *ctx);
