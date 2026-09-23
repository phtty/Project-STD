/**
 * @file    pl_exti.h
 * @brief   EXTI 外部中断 Platform 层抽象接口
 */

#pragma once

#include <stdint.h>

/** @brief EXTI 回调（pin=触发引脚, ctx=用户上下文） */
typedef void (*pl_exti_fn_t)(uint16_t pin, void *ctx);

/** @brief 初始化外部中断引脚配置（hw_pl_initcall 阶段） */
void pl_exti_init(void);

/** @brief 注册某引脚的外部中断回调
 *  @param pin 触发引脚（EXTI 线号）
 *  @param cb  中断回调；传 NULL 表示注销
 *  @param ctx 回调透传的用户上下文 */
void pl_exti_register_cb(uint16_t pin, pl_exti_fn_t cb, void *ctx);
