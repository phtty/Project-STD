/**
 * @file    pl_iwdg.h
 * @brief   独立看门狗抽象接口
 */

#pragma once

#include <stdbool.h>

/** @brief IWDG 不透明句柄 */
typedef void *pl_iwdg_handle_t;

void            pl_iwdg_init(void);
pl_iwdg_handle_t pl_iwdg_get_handle(void);

/** @brief 刷新看门狗计数器，防止系统复位 */
void pl_iwdg_refresh(pl_iwdg_handle_t h);
