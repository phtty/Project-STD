/**
 * @file    task.h
 * @brief   host 单测用的 task.h 替身
 *
 * 真机里 configASSERT 会调用 taskDISABLE_INTERRUPTS()；本工程的 FreeRTOS.h 替身
 * 已把 configASSERT 换成 abort，这里只需让 include 能解析。
 */

#pragma once

#include "FreeRTOS.h"

#define taskDISABLE_INTERRUPTS() ((void)0)
#define taskENABLE_INTERRUPTS()  ((void)0)
#define taskENTER_CRITICAL()     ((void)0)
#define taskEXIT_CRITICAL()      ((void)0)
