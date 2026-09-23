/**
 * @file    initcall.h
 * @brief   自动初始化框架 — hw (RTOS前) / sw (RTOS后) 双段，按分层排列
 *
 * 执行顺序由层级序号保证：
 *   hw: pre(0) → pl(1) → dev(2) → post(3)
 *   sw: pre(0) → pl(1) → dev(2) → app(3) → post(4)
 *   pl = Platform 层, dev = Device 层, app = Application 层
 *   同层内的相对次序**不可依赖**，理由见下。
 *
 * 关于"同层顺序"：链接脚本写的是 KEEP(*(SORT(.sw_initcall.0))) —— SORT 排的是
 * **输入段名**，而宏生成的段名是 section(".sw_initcall." \#lvl)，不含函数名。
 * 同层所有条目的段名完全相同，排序即空操作，剩下的就是目标文件在链接命令里的
 * 先后，也就是构建清单的文件次序（Makefile 与 eide.yml 各有一份，两者未必一致）。
 * 想表达依赖请用**层级**（如把"读配置"放到 sw_post(4) 让"加载配置"sw_app(3) 先跑），
 * 不要靠调整清单顺序。
 */

#pragma once

#include <stdint.h>

/** @brief initcall 条目函数类型 */
typedef void (*initcall_fn_t)(void);

/** @brief initcall 表项 —— 函数指针 + 供日志/排障用的名字 */
typedef struct {
    initcall_fn_t fn;   /**< 被注册的初始化函数 */
    const char *name;   /**< 函数名（编译期取 \#fn）*/
} initcall_entry_t;

/* ---- 硬件 initcall（main.c 中 initcall_run 调用，RTOS 前） ---- */
/** @brief 硬件 initcall 底层声明宏（按层级 lvl 归段）*/
#define OS_HWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".hw_initcall." #lvl))) \
    __hw_initcall_##lvl##_##fn = { (initcall_fn_t)(fn), #fn }

/** @brief 注册 hw pre 层（0）初始化 */
#define hw_pre_initcall(fn)    OS_HWINITCALL(0, fn)
/** @brief 注册 hw Platform 层（1）初始化 */
#define hw_pl_initcall(fn)     OS_HWINITCALL(1, fn)
/** @brief 注册 hw Device 层（2）初始化 */
#define hw_dev_initcall(fn)    OS_HWINITCALL(2, fn)
/** @brief 注册 hw post 层（3）初始化 */
#define hw_post_initcall(fn)   OS_HWINITCALL(3, fn)

/* ---- 软件 initcall（init_task 中 initcall_run_sw 调用，RTOS 后） ---- */
/** @brief 软件 initcall 底层声明宏（按层级 lvl 归段）*/
#define OS_SWINITCALL(lvl, fn) \
    static const initcall_entry_t __attribute__((used, section(".sw_initcall." #lvl))) \
    __sw_initcall_##lvl##_##fn = { (initcall_fn_t)(fn), #fn }

/** @brief 注册 sw pre 层（0）初始化 */
#define sw_pre_initcall(fn)    OS_SWINITCALL(0, fn)
/** @brief 注册 sw Platform 层（1）初始化 */
#define sw_pl_initcall(fn)     OS_SWINITCALL(1, fn)
/** @brief 注册 sw Device 层（2）初始化 */
#define sw_dev_initcall(fn)    OS_SWINITCALL(2, fn)
/** @brief 注册 sw Application 层（3）初始化 */
#define sw_app_initcall(fn)    OS_SWINITCALL(3, fn)
/** @brief 注册 sw post 层（4）初始化 */
#define sw_post_initcall(fn)   OS_SWINITCALL(4, fn)

/* ---- 边界符号 ---- */
extern const initcall_entry_t __hw_initcall_start[]; /**< .hw_initcall 段起始 */
extern const initcall_entry_t __hw_initcall_end[];   /**< .hw_initcall 段结束 */
extern const initcall_entry_t __sw_initcall_start[]; /**< .sw_initcall 段起始 */
extern const initcall_entry_t __sw_initcall_end[];   /**< .sw_initcall 段结束 */

/** @brief 遍历 [start, end) 依次调用各 initcall 条目 */
void initcall_run(const initcall_entry_t *start, const initcall_entry_t *end);
/** @brief 遍历全部软件 initcall（.sw_initcall 段）*/
void initcall_run_sw(void);
