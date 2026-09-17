/**
 * @file    pl_task.c
 * @brief   任务创建薄封装实现（见 pl_task.h）
 */

#include "pl_task.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

static uint16_t s_fail_cnt;

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    osThreadId_t id = osThreadNew(fn, arg, attr);

    if (id == NULL) {
        s_fail_cnt++;
        /* 经 printf → RTT 输出（见 SEGGER_RTT_Syscalls_GCC.c 的 _write 重定向）。
           堆余量是关键信息：它直接说明"差多少"，而不是只告诉你"失败了"。 */
        printf("[pl_task] 任务 %s 创建失败（第 %u 次），堆余量 %u 字节\n",
               (attr != NULL && attr->name != NULL) ? attr->name : "?", (unsigned)s_fail_cnt,
               (unsigned)xPortGetFreeHeapSize());
    }
    return id;
}

uint16_t pl_task_fail_count(void)
{
    return s_fail_cnt;
}
