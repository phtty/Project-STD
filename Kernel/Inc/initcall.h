/**
 * @file    initcall.h
 * @brief   自动初始化框架 — hw (RTOS前) / sw (RTOS后) 双段，按分层排列
 *
 * 执行顺序由层级序号保证：
 *   hw: pre(0) → pl(1) → dev(2) → post(3)
 *   sw: pre(0) → pl(1) → dev(2) → app(3) → post(4)
 *   pl = Platform 层, dev = Device 层, app = Application 层
 *   同层内按符号名字母序排列
 */

#pragma once

#include <stdint.h>

typedef void (*initcall_fn)(void);

typedef struct {
    initcall_fn fn;
    const char *name;
} initcall_entry_t;

/* ---- 硬件 initcall（main.c 中 initcall_run 调用，RTOS 前） ---- */
#define OS_HWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".hw_initcall." #lvl))) \
    __hw_initcall_##lvl##_##fn = { (initcall_fn)(fn), #fn }

#define hw_pre_initcall(fn)    OS_HWINITCALL(0, fn)
#define hw_pl_initcall(fn)     OS_HWINITCALL(1, fn)
#define hw_dev_initcall(fn)    OS_HWINITCALL(2, fn)
#define hw_post_initcall(fn)   OS_HWINITCALL(3, fn)

/* ---- 软件 initcall（init_task 中 sw_board_init 调用，RTOS 后） ---- */
#define OS_SWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".sw_initcall." #lvl))) \
    __sw_initcall_##lvl##_##fn = { (initcall_fn)(fn), #fn }

#define sw_pre_initcall(fn)    OS_SWINITCALL(0, fn)
#define sw_pl_initcall(fn)     OS_SWINITCALL(1, fn)
#define sw_dev_initcall(fn)    OS_SWINITCALL(2, fn)
#define sw_app_initcall(fn)    OS_SWINITCALL(3, fn)
#define sw_post_initcall(fn)   OS_SWINITCALL(4, fn)

/* ---- 边界符号 ---- */
extern const initcall_entry_t __hw_initcall_start[];
extern const initcall_entry_t __hw_initcall_end[];
extern const initcall_entry_t __sw_initcall_start[];
extern const initcall_entry_t __sw_initcall_end[];

void initcall_run(const initcall_entry_t *start, const initcall_entry_t *end);
void sw_board_init(void);
