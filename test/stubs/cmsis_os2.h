/**
 * @file    cmsis_os2.h
 * @brief   host 单测用的 cmsis_os2 替身
 *
 * 只提供被测代码实际引用到的符号。**未实现的原语故意不提供**——链接期失败
 * 优于返回一个看起来正常的假值。
 *
 * 本替身不做真正的互斥：测试一律给 rb API 传 mutex = nullptr（不触锁路径），
 * 需要并发语义的用例应改用 pthread 实现（见参考工程 test/stubs/os_stub.c）。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void *osMutexId_t;
typedef uint32_t osStatus_t;

typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
} osMutexAttr_t;

#define osMutexPrioInherit (1U << 0)
#define osWaitForever      (0xFFFFFFFFU)

osMutexId_t osMutexNew(const osMutexAttr_t *attr);
osStatus_t osMutexAcquire(osMutexId_t id, uint32_t timeout);
osStatus_t osMutexRelease(osMutexId_t id);
