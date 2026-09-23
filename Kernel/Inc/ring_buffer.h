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

/**
 * @brief 编译期静态声明一个环形缓冲区实例，可指定数据区的段属性
 * @param name  实例名
 * @param sz    容量（字节），应为 2 的幂以优化取模
 * @param attr  数据区的属性（如平台提供的 PL_CCMRAM）；无则留空
 *
 * 数据区与属性分开传，是为了让本文件保持平台无关 —— 段名是链接脚本的事，
 * 不该由 Kernel 层规定。
 */
#define RB_DEFINE_ATTR(name, sz, attr) \
    static uint8_t name##_buf[sz] attr; \
    ring_buffer_t name = {.data = name##_buf, .size = sz, .mutex = nullptr}

/**
 * @brief 编译期静态声明一个环形缓冲区实例
 * @param name  实例名
 * @param sz    容量（字节），应为 2 的幂以优化取模
 */
#define RB_DEFINE(name, sz) RB_DEFINE_ATTR(name, sz, )

/** @brief 环形缓冲区结构体 */
typedef struct {
    uint8_t *data;                 /**< 数据区（编译期静态分配）*/
    uint16_t size;                 /**< 容量（字节）*/
    volatile uint16_t read_index;  /**< 读索引 */
    volatile uint16_t write_index; /**< 写索引 */
    void *mutex; /**< osMutexId_t，nullptr 表示未绑定锁 */
} ring_buffer_t;

/** @brief 运行时初始化：创建并绑定优先级继承互斥锁
 *  @param[in,out] rb 环形缓冲区；写入 mutex
 *  @param name 互斥锁名（RTOS 排障用） */
void rb_init(ring_buffer_t *rb, const char *name);

/** @brief 手动加锁（多步原子序列开始时调用） */
void rb_lock(const ring_buffer_t *rb);

/** @brief 手动解锁（多步原子序列结束时调用） */
void rb_unlock(const ring_buffer_t *rb);

/* ---- 环形缓冲区 API（mutex 参数控制是否加锁）---- */

/* 状态查询 */
/** @brief 缓冲区是否为空
 *  @return true = 无数据可读 */
bool rb_empty(const ring_buffer_t *rb, void *mutex);
/** @brief 缓冲区是否已满
 *  @return true = 无空间可写 */
bool rb_full(const ring_buffer_t *rb, void *mutex);
/** @brief 可读字节数
 *  @return 可读字节数 */
uint16_t rb_avail(const ring_buffer_t *rb, void *mutex);
/** @brief 可写字节数
 *  @return 可写字节数 */
uint16_t rb_space(const ring_buffer_t *rb, void *mutex);
/** @brief 丢弃全部未读数据（读索引追上写索引）
 *  @param[in,out] rb 环形缓冲区
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁） */
void rb_flush(ring_buffer_t *rb, void *mutex);

/* 写入 */
/** @brief 写入一个字节
 *  @param[in,out] rb 环形缓冲区
 *  @param byte 待写入的字节
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return true = 写入成功；false = 缓冲区已满 */
bool rb_putc(ring_buffer_t *rb, uint8_t byte, void *mutex);
/** @brief 写入一批字节
 *  @param[in,out] rb 环形缓冲区
 *  @param data 待写数据
 *  @param len  想写入的字节数
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return 实际写入的字节数（<= len，空间不足时截断）*/
uint16_t rb_write(ring_buffer_t *rb, const uint8_t *data, uint16_t len, void *mutex);

/* 读取 */
/** @brief 读出一个字节（移动读指针）
 *  @param[in,out] rb 环形缓冲区
 *  @param[out] byte 接收读出的字节
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return true = 读到一个字节；false = 缓冲区为空 */
bool rb_getc(ring_buffer_t *rb, uint8_t *byte, void *mutex);
/** @brief 读出一批字节（移动读指针）
 *  @param[in,out] rb 环形缓冲区
 *  @param[out] data 接收缓冲
 *  @param len  想读出的字节数
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return 实际读出的字节数 */
uint16_t rb_read(ring_buffer_t *rb, uint8_t *data, uint16_t len, void *mutex);

/* 窥视（不移动读指针） */

/**
 * @brief 窥视至多 dest_cap 字节到 dest，返回实际拷出的字节数
 *
 * 与 rb_peek 的区别：rb_peek 的 len 是"想要的字节数"，只按缓冲区自身容量夹紧，
 * **不感知 dest 有多大** —— 调用方若拿一个小于缓冲区的栈数组当 dest，就会越界写。
 * 本函数把 dest_cap 当作硬上限，装不下就只拷贝 dest_cap 字节（返回值即拷出量）。
 *
 * 帧探测必须用本函数：帧长超过暂存区时，调用方据返回值判定"装不下"并整帧丢弃，
 * 而不是让拷贝写穿暂存区。
 *
 *  @param rb   环形缓冲区
 *  @param offset 起始偏移（0 = 下一个待读字节）
 *  @param[out] dest 接收缓冲
 *  @param dest_cap 接收缓冲容量；拷贝量以此硬上限夹紧
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return 实际拷出的字节数（<= dest_cap）
 */
uint16_t rb_peek_capped(const ring_buffer_t *rb, uint16_t offset, uint8_t *dest, uint16_t dest_cap,
                        void *mutex);

/** @brief 窥视一个字节（不移动读指针）
 *  @param rb   环形缓冲区
 *  @param offset 起始偏移（0 = 下一个待读字节）
 *  @param[out] byte 接收窥视到的字节
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return true = 该 offset 处有字节；false = 越界 */
bool rb_peekc(const ring_buffer_t *rb, uint16_t offset, uint8_t *byte, void *mutex);
/** @brief 窥视至多 len 字节（不移动读指针，只按缓冲区容量夹紧）
 *  @param rb   环形缓冲区
 *  @param offset 起始偏移（0 = 下一个待读字节）
 *  @param[out] dest 接收缓冲
 *  @param len 想要的字节数
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return 实际拷出的字节数 */
uint16_t rb_peek(const ring_buffer_t *rb, uint16_t offset, uint8_t *dest, uint16_t len, void *mutex);

/* 工具 */
/** @brief 从 offset 起、可不跨回绕连续读取的字节数
 *  @return 连续字节数；offset 越界返回 0 */
uint16_t rb_contig(const ring_buffer_t *rb, uint16_t offset, void *mutex);
/** @brief 丢弃 len 字节（移动读指针）
 *  @param[in,out] rb 环形缓冲区
 *  @param len 想丢弃的字节数
 *  @param mutex 锁参数：传 rb->mutex 自动加解锁，传 nullptr 跳过（调用者持锁）
 *  @return 实际丢弃的字节数（<= len）*/
uint16_t rb_skip(ring_buffer_t *rb, uint16_t len, void *mutex);
