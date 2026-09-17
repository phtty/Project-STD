/**
 * @file    FreeRTOS.h
 * @brief   host 单测用的 FreeRTOS 替身
 *
 * 真机由 Middlewares/Third_Party/FreeRTOS 提供；host 测试把 test/stubs 放在
 * include 路径最前，只提供被测代码实际引用到的少量定义。
 * 优先命中本文件而不是真头文件是刻意的 —— 真 FreeRTOS.h 会拉进 portmacro /
 * 汇编移植层与 FreeRTOSConfig.h，无法在 host 上编译，而分发引擎本身并不需要它们。
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>

/**
 * 真机的 configASSERT 会关中断并死循环；host 上直接 abort，
 * 让断言失败以崩溃的形式暴露给测试，而不是把整个测试挂住。
 */
#define configASSERT(x)                                                       \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "configASSERT 失败: %s (%s:%d)\n", #x, __FILE__,  \
                    __LINE__);                                                \
            abort();                                                          \
        }                                                                     \
    } while (0)

#define configASSERT_DEFINED 1

/* 真机定义在 FreeRTOS.h 内；host 侧不使用静态对象内存，只求类型存在 */
typedef struct {
    void *dummy1;
    void *dummy2;
} StaticQueue_t;

typedef StaticQueue_t StaticSemaphore_t;

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef unsigned int TickType_t;

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE
