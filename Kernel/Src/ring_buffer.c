/**
 * @file    ring_buffer.c
 * @brief   环形缓冲区（FIFO）实现，通过 mutex 参数控制线程安全
 *
 * 编译期 RB_DEFINE(name, size) 声明实例，内存完全静态分配。
 * 单生产者/单消费者模型，取模运算回绕索引。
 *
 * 每个 API 末尾的 void *mutex 参数：
 *   传 rb->mutex → 自动加锁/解锁（单步操作）
 *   传 nullptr   → 跳过锁，调用者自行持锁（多步原子序列）
 * 内部相互调用一律传 nullptr，避免同一线程重复加锁导致死锁。
 */

#include "ring_buffer.h"
#include "cmsis_os2.h"
#include <string.h>

/* ================================================================
 *  锁管理
 * ================================================================ */

void rb_init(ring_buffer_t *rb, const char *name)
{
    osMutexAttr_t attr = {
        .name      = name,
        .attr_bits = osMutexPrioInherit,
    };
    rb->mutex = osMutexNew(&attr);
}

void rb_lock(const ring_buffer_t *rb)
{
    if (rb->mutex)
        osMutexAcquire(rb->mutex, osWaitForever);
}

void rb_unlock(const ring_buffer_t *rb)
{
    if (rb->mutex)
        osMutexRelease(rb->mutex);
}

/* ================================================================
 *  API 实现（mutex 参数控制是否加锁）
 * ================================================================ */

/* ---- 状态查询 ---- */

bool rb_empty(const ring_buffer_t *rb, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    bool r = (rb->read_index == rb->write_index);
    if (mutex) osMutexRelease(mutex);
    return r;
}

bool rb_full(const ring_buffer_t *rb, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    bool r = ((rb->write_index + 1) % rb->size) == rb->read_index;
    if (mutex) osMutexRelease(mutex);
    return r;
}

uint16_t rb_avail(const ring_buffer_t *rb, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    /* C 整数提升陷阱：uint16_t 相减前先提升为有符号 int，负数 % 正数 = 负数，
     * 再转回 uint16_t 时不取模而是回绕成超大值。用 uint32_t 强制无符号运算。 */
    uint16_t r = (uint16_t)(((uint32_t)rb->write_index - (uint32_t)rb->read_index) % rb->size);
    if (mutex) osMutexRelease(mutex);
    return r;
}

uint16_t rb_space(const ring_buffer_t *rb, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t r = rb->size - rb_avail(rb, nullptr) - 1;
    if (mutex) osMutexRelease(mutex);
    return r;
}

void rb_flush(ring_buffer_t *rb, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    rb->read_index = rb->write_index;
    if (mutex) osMutexRelease(mutex);
}

/* ---- 写入 ---- */

bool rb_putc(ring_buffer_t *rb, uint8_t byte, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    if (rb_full(rb, nullptr)) {
        if (mutex) osMutexRelease(mutex);
        return false;
    }
    rb->data[rb->write_index] = byte;
    rb->write_index           = (rb->write_index + 1) % rb->size;
    if (mutex) osMutexRelease(mutex);
    return true;
}

uint16_t rb_write(ring_buffer_t *rb, const uint8_t *data, uint16_t len, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t free = rb_space(rb, nullptr);
    if (len > free) len = free;
    if (len > rb->size) len = rb->size; /* 防御：len 绝不应超过缓冲区容量 */
    if (len == 0) {
        if (mutex) osMutexRelease(mutex);
        return 0;
    }

    uint16_t start      = rb->write_index;
    uint16_t contiguous = rb->size - start;

    if (len <= contiguous) {
        memcpy(&rb->data[start], data, len);
    } else {
        memcpy(&rb->data[start], data, contiguous);
        memcpy(rb->data, data + contiguous, len - contiguous);
    }
    rb->write_index = (start + len) % rb->size;
    if (mutex) osMutexRelease(mutex);
    return len;
}

/* ---- 读取 ---- */

bool rb_getc(ring_buffer_t *rb, uint8_t *byte, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    if (rb_empty(rb, nullptr)) {
        if (mutex) osMutexRelease(mutex);
        return false;
    }
    *byte          = rb->data[rb->read_index];
    rb->read_index = (rb->read_index + 1) % rb->size;
    if (mutex) osMutexRelease(mutex);
    return true;
}

uint16_t rb_read(ring_buffer_t *rb, uint8_t *data, uint16_t len, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t avail = rb_avail(rb, nullptr);
    if (len > avail) len = avail;
    if (len > rb->size) len = rb->size; /* 防御：len 绝不应超过缓冲区容量 */
    if (len == 0) {
        if (mutex) osMutexRelease(mutex);
        return 0;
    }

    uint16_t start      = rb->read_index;
    uint16_t contiguous = rb->size - start;

    if (len <= contiguous) {
        memcpy(data, &rb->data[start], len);
    } else {
        memcpy(data, &rb->data[start], contiguous);
        memcpy(data + contiguous, rb->data, len - contiguous);
    }
    rb->read_index = (start + len) % rb->size;
    if (mutex) osMutexRelease(mutex);
    return len;
}

/* ---- 窥视（不移动读指针）---- */

bool rb_peekc(const ring_buffer_t *rb, uint16_t offset, uint8_t *byte, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t avail = rb_avail(rb, nullptr);
    if (offset >= avail) {
        if (mutex) osMutexRelease(mutex);
        return false;
    }
    uint16_t index = (rb->read_index + offset) % rb->size;
    *byte          = rb->data[index];
    if (mutex) osMutexRelease(mutex);
    return true;
}

uint16_t rb_peek(const ring_buffer_t *rb, uint16_t offset, uint8_t *dest, uint16_t len, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t avail = rb_avail(rb, nullptr);
    if (offset >= avail) {
        if (mutex) osMutexRelease(mutex);
        return 0;
    }
    if (len > avail - offset) len = avail - offset;
    if (len > rb->size) len = rb->size; /* 防御：len 绝不应超过缓冲区容量 */

    uint16_t start      = (rb->read_index + offset) % rb->size;
    uint16_t contiguous = rb->size - start;

    if (len <= contiguous) {
        memcpy(dest, &rb->data[start], len);
    } else {
        memcpy(dest, &rb->data[start], contiguous);
        memcpy(dest + contiguous, rb->data, len - contiguous);
    }
    if (mutex) osMutexRelease(mutex);
    return len;
}

/* ---- 工具 ---- */

uint16_t rb_contig(const ring_buffer_t *rb, uint16_t offset, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t avail = rb_avail(rb, nullptr);
    if (offset >= avail) {
        if (mutex) osMutexRelease(mutex);
        return 0;
    }

    uint16_t start      = (rb->read_index + offset) % rb->size;
    uint16_t contiguous = rb->size - start;
    uint16_t remaining  = avail - offset;

    uint16_t r = (contiguous < remaining) ? contiguous : remaining;
    if (mutex) osMutexRelease(mutex);
    return r;
}

uint16_t rb_skip(ring_buffer_t *rb, uint16_t len, void *mutex)
{
    if (mutex) osMutexAcquire(mutex, osWaitForever);
    uint16_t avail = rb_avail(rb, nullptr);
    if (len > avail) len = avail;
    rb->read_index = (rb->read_index + len) % rb->size;
    if (mutex) osMutexRelease(mutex);
    return len;
}
