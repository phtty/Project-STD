/**
 * @file    os_stub.c
 * @brief   cmsis_os2 的 pthread 替身 —— 让分发引擎原样跑在 host 上
 *
 * 目的：app_dispatch.c / ring_buffer.c / 各协议模块 **不做任何改动**就能在 host
 * 编译运行，因此测的是生产代码本身，而不是它的复制品。
 *
 * 语义上刻意贴近 FreeRTOS：
 *   - 互斥量非递归且带检错（PTHREAD_MUTEX_ERRORCHECK）—— 与 osMutexPrioInherit
 *     的非递归语义一致，重复加锁会立刻报错而不是悄悄成功或挂死；
 *   - 队列元素按调用方给的 mq_mem 静态缓冲区存放，写超边界会被 ASan 直接抓到；
 *   - 1 tick = 1 ms。
 *
 * **未实现的原语一旦被引用就会在链接期失败** —— 这比返回一个看起来正常的假值
 * 更安全（本替身只提供 osMessageQueue / osMutex / osThread / osDelay 四类）。
 */

#include "cmsis_os2.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================
 *  内部结构
 * ================================================================ */

typedef struct {
    uint8_t        *buf; /**< 元素存储区（调用方 mq_mem 或堆分配）*/
    uint32_t        item_size;
    uint32_t        count;
    uint32_t        head;
    uint32_t        used;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} stub_queue_t;

typedef struct {
    pthread_mutex_t mtx;
} stub_mutex_t;

/* ================================================================
 *  时间辅助
 * ================================================================ */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void deadline_from(uint32_t ticks, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    uint64_t ns = (uint64_t)ts->tv_nsec + (uint64_t)ticks * 1000000ULL;
    ts->tv_sec += (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

/* ================================================================
 *  消息队列
 * ================================================================ */

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    if (msg_count == 0 || msg_size == 0) return NULL;

    stub_queue_t *q = (stub_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL) return NULL;

    q->item_size = msg_size;
    q->count     = msg_count;
    q->buf       = (attr != NULL && attr->mq_mem != NULL)
                       ? (uint8_t *)attr->mq_mem
                       : (uint8_t *)calloc(msg_count, msg_size);
    if (q->buf == NULL) {
        free(q);
        return NULL;
    }

    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return (osMessageQueueId_t)q;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr, uint8_t msg_prio,
                             uint32_t timeout)
{
    (void)msg_prio;
    stub_queue_t *q = (stub_queue_t *)mq_id;
    if (q == NULL || msg_ptr == NULL) return osErrorParameter;

    pthread_mutex_lock(&q->mtx);
    while (q->used == q->count) {
        if (timeout == 0) {
            pthread_mutex_unlock(&q->mtx);
            return osErrorResource;
        }
        if (timeout == osWaitForever) {
            pthread_cond_wait(&q->not_full, &q->mtx);
        } else {
            struct timespec ts;
            deadline_from(timeout, &ts);
            if (pthread_cond_timedwait(&q->not_full, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return osErrorTimeout;
            }
        }
    }

    uint32_t tail = (q->head + q->used) % q->count;
    memcpy(q->buf + (size_t)tail * q->item_size, msg_ptr, q->item_size);
    q->used++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr, uint8_t *msg_prio,
                             uint32_t timeout)
{
    (void)msg_prio;
    stub_queue_t *q = (stub_queue_t *)mq_id;
    if (q == NULL || msg_ptr == NULL) return osErrorParameter;

    pthread_mutex_lock(&q->mtx);
    while (q->used == 0) {
        if (timeout == 0) {
            pthread_mutex_unlock(&q->mtx);
            return osErrorResource;
        }
        if (timeout == osWaitForever) {
            pthread_cond_wait(&q->not_empty, &q->mtx);
        } else {
            struct timespec ts;
            deadline_from(timeout, &ts);
            if (pthread_cond_timedwait(&q->not_empty, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return osErrorTimeout;
            }
        }
    }

    memcpy(msg_ptr, q->buf + (size_t)q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->count;
    q->used--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return osOK;
}

/* ================================================================
 *  互斥量
 * ================================================================ */

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    (void)attr;
    stub_mutex_t *m = (stub_mutex_t *)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m->mtx, &ma);
    pthread_mutexattr_destroy(&ma);
    return (osMutexId_t)m;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    stub_mutex_t *m = (stub_mutex_t *)mutex_id;
    if (m == NULL) return osErrorParameter;

    if (timeout == 0)
        return pthread_mutex_trylock(&m->mtx) == 0 ? osOK : osErrorResource;

    int rc = pthread_mutex_lock(&m->mtx);
    if (rc == EDEADLK) {
        /* 真机的非递归互斥量在这里同样会死锁 —— 提前报错，别让测试挂住 */
        fprintf(stderr, "osMutexAcquire: 同一线程重复加锁（非递归互斥量）\n");
        abort();
    }
    return rc == 0 ? osOK : osError;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    stub_mutex_t *m = (stub_mutex_t *)mutex_id;
    if (m == NULL) return osErrorParameter;
    return pthread_mutex_unlock(&m->mtx) == 0 ? osOK : osError;
}

/* ================================================================
 *  线程
 * ================================================================ */

typedef struct {
    osThreadFunc_t fn;
    void          *arg;
} thread_tramp_t;

static void *thread_trampoline(void *p)
{
    thread_tramp_t t = *(thread_tramp_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    (void)attr;
    if (func == NULL) return NULL;

    thread_tramp_t *t = (thread_tramp_t *)malloc(sizeof(*t));
    if (t == NULL) return NULL;
    t->fn  = func;
    t->arg = argument;

    pthread_t tid;
    if (pthread_create(&tid, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return (osThreadId_t)(uintptr_t)tid;
}

void osThreadExit(void)
{
    pthread_exit(NULL);
}

/* ================================================================
 *  时间
 * ================================================================ */

osStatus_t osDelay(uint32_t ticks)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ticks / 1000U),
        .tv_nsec = (long)(ticks % 1000U) * 1000000L,
    };
    nanosleep(&ts, NULL);
    return osOK;
}

uint32_t osKernelGetTickCount(void)
{
    return (uint32_t)now_ms();
}

/* ---- FreeRTOS 侧 ---- */

/** 真机返回 heap_4 的可用字节数（pl_task.c 在任务创建失败时打进日志）。
 *  host 上没有堆管理器，返回一个固定值 —— 被测逻辑只把它当数字打出来。 */
size_t xPortGetFreeHeapSize(void)
{
    return 32768U;
}
