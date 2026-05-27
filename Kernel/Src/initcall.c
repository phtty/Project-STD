/**
 * @file    initcall.c
 * @brief   软件 initcall 遍历入口
 *
 * sw_board_init() — RTOS 后：遍历 .sw_initcall
 * board_init() 已移至 Core/Src/main.c（需 HAL/Platform 调用，Kernel 层不应依赖上层）
 */

#include "initcall.h"

void initcall_run(const initcall_entry_t *start, const initcall_entry_t *end)
{
    for (const initcall_entry_t *p = start; p < end; p++)
        if (p->fn) p->fn();
}

void sw_board_init(void)
{
    initcall_run(__sw_initcall_start, __sw_initcall_end);
}
