/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : 主入口
 ******************************************************************************
 */

#include <stdio.h>

#include "main.h"
#include "initcall.h"
#include "pl_sys.h"

void app_boot(void);

int main(void)
{
    SCB->VTOR = FLASH_BASE | 0x40000;
    __enable_irq();

    HAL_Init();
    SystemClock_Config();

    initcall_run(__hw_initcall_start, __hw_initcall_end);

    /* 边界检查：hw 层跑完中断必须是开的。
     *
     * 这一层最容易踩的坑是"在 hw initcall 里创建 RTOS 内核对象"——那要从堆上
     * 分配，而 heap_4 的 pvPortMalloc 会进临界区（taskENTER/EXIT_CRITICAL）。
     * 调度器启动前 uxCriticalNesting 是哨兵值 0xaaaaaaaa，只在
     * xPortStartScheduler() 里被置 0；此前进一次临界区，退出时递减成
     * 0xaaaaaaa9 ≠ 0，portENABLE_INTERRUPTS() 就永远不被调用 —— 中断从此永久
     * 关闭，TIM7 停摆（HAL 时基不走），HAL_Delay 死等。
     *
     * 这个失败是**静默**的：configASSERT(uxCriticalNesting) 只判非零，哨兵值
     * 恰好非零，断言不会报。所以在边界上直接查一次，把它变成一声明确的报错。
     * （内核对象请在 sw_* initcall 里创建：RTOS 已启动，且同样早于任何任务。） */
    if (__get_BASEPRI() != 0U) {
        printf("\n[main] hw initcall 结束后中断处于关闭状态（BASEPRI=0x%02X）——"
               "多半是有 hw 层代码创建了 RTOS 内核对象。内核对象请放到 sw_* initcall。\n",
               (unsigned)__get_BASEPRI());
        __enable_irq();
    }

    app_boot();

    for (;;) {}
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
