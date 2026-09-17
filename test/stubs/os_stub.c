/**
 * @file    os_stub.c
 * @brief   cmsis_os2 互斥量替身（host 单测）
 *
 * 当前只覆盖 ring_buffer 的锁路径：本测试全部传 mutex = nullptr，不触锁，
 * 故这里只需让符号可链接。真要测并发语义时应换成 pthread 实现。
 */

#include "cmsis_os2.h"

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    (void)attr;
    return nullptr;
}

osStatus_t osMutexAcquire(osMutexId_t id, uint32_t timeout)
{
    (void)id;
    (void)timeout;
    return 0;
}

osStatus_t osMutexRelease(osMutexId_t id)
{
    (void)id;
    return 0;
}
