/**
 * @file    initcall.h
 * @brief   Linux 风格 initcall 自动初始化框架——硬件/软件双段分离
 *
 * .hw_initcall（board_init 调用，RTOS 前）：GPIO、DMA、UART、ETH 等纯硬件初始化
 * .sw_initcall（sw_board_init 调用，RTOS 后）：调度框架、协议注册、通道 RTOS 资源
 */

#pragma once

#include <stdint.h>

typedef void (*initcall_fn)(void);

typedef struct {
    initcall_fn fn;
    const char *name;
} initcall_entry_t;

/* ---- 硬件 initcall（RTOS 前） ---- */
#define OS_HWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".hw_initcall." #lvl))) \
    __hw_initcall_##lvl##_##fn = { (initcall_fn)(fn), #fn }

#define hw_arch_initcall(fn)     OS_HWINITCALL(1, fn)
#define hw_subsys_initcall(fn)   OS_HWINITCALL(2, fn)
#define hw_device_initcall(fn)   OS_HWINITCALL(3, fn)
#define hw_driver_initcall(fn)   OS_HWINITCALL(4, fn)
#define hw_late_initcall(fn)     OS_HWINITCALL(5, fn)

/* ---- 软件 initcall（RTOS 后，init_task 中 sw_board_init 调用） ---- */
#define OS_SWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".sw_initcall." #lvl))) \
    __sw_initcall_##lvl##_##fn = { (initcall_fn)(fn), #fn }

#define sw_arch_initcall(fn)     OS_SWINITCALL(1, fn)
#define sw_subsys_initcall(fn)   OS_SWINITCALL(2, fn)
#define sw_device_initcall(fn)   OS_SWINITCALL(3, fn)
#define sw_driver_initcall(fn)   OS_SWINITCALL(4, fn)
#define sw_late_initcall(fn)     OS_SWINITCALL(5, fn)

/* ---- 边界符号 ---- */
extern const initcall_entry_t __hw_initcall_start[];
extern const initcall_entry_t __hw_initcall_end[];
extern const initcall_entry_t __sw_initcall_start[];
extern const initcall_entry_t __sw_initcall_end[];

void initcall_run(const initcall_entry_t *start, const initcall_entry_t *end);
void sw_board_init(void);
