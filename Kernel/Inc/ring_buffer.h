/**
 * @file    ring_buffer.h
 * @brief   环形缓冲区 — 编译期静态分配，通过 mutex 参数控制锁行为
 *
 * RB_DEFINE(name, size) 在编译期声明实例（栈/全局/static 均可），
 * 数据区和控制结构连续分配，零堆开销。
 *
 * 线程安全：
 *   每个 API 末尾接受 void *mutex 参数：
 *   - 传 rb->mutex：自动加锁/解锁（单步操作）
 *   - 传 nullptr：  跳过锁，调用者自行持锁（多步原子序列）
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 编译期静态声明一个环形缓冲区实例
 * @param name  实例名
 * @param sz    容量（字节），应为 2 的幂以优化取模
 */
#define RB_DEFINE(name, sz) \
    static uint8_t name##_buf[sz]; \
    ring_buffer_t name = { .data = name##_buf, .size = sz, .mutex = nullptr }

/** @brief 环形缓冲区结构体 */
typedef struct {
    uint8_t *data;
    uint16_t size;
    volatile uint16_t read_index;
    volatile uint16_t write_index;
    void *mutex; /**< osMutexId_t，nullptr 表示未绑定锁 */
} ring_buffer_t;

/** @brief 运行时初始化：创建并绑定优先级继承互斥锁 */
void rb_init(ring_buffer_t *rb, const char *name);

/** @brief 手动加锁（多步原子序列开始时调用） */
void rb_lock(const ring_buffer_t *rb);

/** @brief 手动解锁（多步原子序列结束时调用） */
void rb_unlock(const ring_buffer_t *rb);

/* ---- 环形缓冲区 API（mutex 参数控制是否加锁）---- */

/* 状态查询 */
bool      rb_empty(const ring_buffer_t *rb, void *mutex);
bool      rb_full(const ring_buffer_t *rb, void *mutex);
uint16_t  rb_avail(const ring_buffer_t *rb, void *mutex);
uint16_t  rb_space(const ring_buffer_t *rb, void *mutex);
void      rb_flush(ring_buffer_t *rb, void *mutex);

/* 写入 */
bool      rb_putc(ring_buffer_t *rb, uint8_t byte, void *mutex);
uint16_t  rb_write(ring_buffer_t *rb, const uint8_t *data, uint16_t len, void *mutex);

/* 读取 */
bool      rb_getc(ring_buffer_t *rb, uint8_t *byte, void *mutex);
uint16_t  rb_read(ring_buffer_t *rb, uint8_t *data, uint16_t len, void *mutex);

/* 窥视（不移动读指针） */
bool      rb_peekc(const ring_buffer_t *rb, uint16_t offset, uint8_t *byte, void *mutex);
uint16_t  rb_peek(const ring_buffer_t *rb, uint16_t offset, uint8_t *dest, uint16_t len, void *mutex);

/* 工具 */
uint16_t  rb_contig(const ring_buffer_t *rb, uint16_t offset, void *mutex);
uint16_t  rb_skip(ring_buffer_t *rb, uint16_t len, void *mutex);
