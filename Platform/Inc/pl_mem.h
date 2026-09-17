/**
 * @file    pl_mem.h
 * @brief   内存区域属性 —— CCMRAM 放置宏与 DMA 可达性检查
 *
 * **CCMRAM（0x10000000，64KB）与 SRAM 的一个关键差别：DMA 够不到它。**
 * CCM 是内核私有的紧耦合内存，直接挂在 DCode 总线上，不经过总线矩阵，
 * 所以任何一个 DMA 控制器都不能把它的地址当源或目标 —— 用了也不会报错，
 * 只是数据不对。
 *
 * 因此凡是会交给 DMA 的缓冲（UART 接收缓冲、SPI DMA 接收目标、以太网
 * 零拷贝收包的 pbuf）**必须留在 SRAM**。
 *
 * 本文件提供两样东西：
 *   1. `PL_CCMRAM` —— 放在支持属性位置的声明后面，即把该对象放进 CCMRAM 段
 *      （`Compiler/STM32F407XX_FLASH.ld` 的 `.ccmram`，NOLOAD，由 startup.c 清零，
 *       行为等同 .bss）；
 *   2. `pl_mem_is_dma_capable()` —— 见下。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 放到 CCMRAM 段。用法：`static uint8_t buf[1024] PL_CCMRAM;` */
#define PL_CCMRAM __attribute__((section(".ccmram")))

/** CCMRAM 段边界，由链接脚本定义（见 .ccmram 段） */
extern uint8_t _sccmram[];
extern uint8_t _eccmram[];

/**
 * @brief 判断一段内存是否可以作为 DMA 的源/目标
 *
 * CCMRAM 不可达，其余（SRAM、以及按地址看不在 CCM 区间内的一切）视为可达。
 *
 * **用途是让约束由代码强制，而不是靠人记着**：把缓冲搬进 CCMRAM 之后，
 * 万一哪天有人把它传给 DMA，这里当场拦下；否则只会表现为"数据不对"，
 * 而且往往在很久以后才被发现。
 */
static inline bool pl_mem_is_dma_capable(const void *ptr, size_t len)
{
    uintptr_t a = (uintptr_t)ptr;
    uintptr_t b = a + len;
    uintptr_t s = (uintptr_t)_sccmram;
    uintptr_t e = (uintptr_t)_eccmram;

    return !(a < e && b > s); /* 与 CCMRAM 区间无交集即可 */
}
