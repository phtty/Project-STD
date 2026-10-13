/**
 * @file    app_diag.c
 * @brief   运行时健康诊断 —— 周期经 RTT 打印各任务栈高水位与堆余量
 *
 * **为什么需要它**：任务栈是按经验拍的。拍多了浪费 SRAM（本工程 SRAM 余量一直
 * 在十几 KB 量级，值得抠），拍少了溢出 —— 而 configCHECK_FOR_STACK_OVERFLOW
 * 只在真的溢出那一刻才报，报出来时已经晚了。FreeRTOS 会记录每个任务历史上的
 * **最小剩余栈**（高水位，单位 StackType_t = 4 字节），据此可以把栈收到有依据的
 * 值，而不是继续拍。
 *
 * 同时打印堆余量与任务创建失败次数：前者是"还能开多少任务"的账，
 * 后者由 pl_task_new 累加（见 pl_task.h 里为什么只记录不阻断）。
 *
 * 输出经 printf → RTT（见 SEGGER_RTT_Syscalls_GCC.c 的 _write 重定向）。
 * RTT 上行缓冲 1KB（SEGGER_RTT_Conf.h）；一次全量打印上限 = APP_DIAG_TASK_MAX(24) 行
 * × 33 字节/行 + 表头 ≈ 0.9KB，当前任务数下约 700 字节（见 _diag_dump 的格式串）。
 * 输出模式是 NO_BLOCK_SKIP，主机侧不持续读取时会被截尾；要完整抓一次就把
 * BUFFER_SIZE_UP 临时调回 4KB。
 *
 * 关闭：把下面 APP_DIAG_ENABLE 置 0。
 */

#define APP_DIAG_ENABLE 0 /* 默认关闭：一次全量打印约 0.9KB，会挤占 1KB 的 RTT 上行缓冲，
                          * 把别的日志挤没了。排障时置 1 打开，打完再置回 0。 */

#if APP_DIAG_ENABLE

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_task.h"
#include "pl_mem.h"
#include "app_udp.h"

#define APP_DIAG_INTERVAL_S (30U) /**< 打印周期（秒）*/
#define APP_DIAG_TASK_MAX   (24U) /**< 最多列出多少个任务 */

/* 放静态而不是栈上：诊断任务不该为了这份数组占 2KB 栈，
   否则它自己的栈高水位反而失去参考价值。 */
static TaskStatus_t s_task_table[APP_DIAG_TASK_MAX] PL_CCMRAM;

static void _diag_dump(void)
{
    UBaseType_t n = uxTaskGetSystemState(s_task_table, APP_DIAG_TASK_MAX, NULL);

    /* 输出刻意紧凑：RTT 上行缓冲只有 1KB（SEGGER_RTT_Conf.h），而模式是
       NO_BLOCK_SKIP —— 一次打印超过缓冲容量就会被**截尾**，上一版就是这样
       在最后几个任务处断掉的。当前约 700 字节，留足余量。
       水位单位是 StackType_t（=4 字节），余量越小越危险；接近 0 就该加栈。 */
    printf("\n[diag] heap=%u fail=%u  (rem: words, x4=bytes)\n", (unsigned)xPortGetFreeHeapSize(),
           (unsigned)pl_task_fail_count());
    printf("[diag] %-16s %3s %4s\n", "task", "pri", "rem");
    /* UDP 计数单列一行：定位"上位机说连上但设备说没收到"这类矛盾时，
       日志会被后面的周期输出刷掉，计数不会。 */
    printf("[diag] udp rx=%lu tx=%lu\n", (unsigned long)app_udp_get_rx_count(),
           (unsigned long)app_udp_get_tx_count());

    for (UBaseType_t i = 0; i < n; i++) {
        if (s_task_table[i].pcTaskName == NULL) continue;
        printf("[diag] %-16s %3u %4u\n", s_task_table[i].pcTaskName,
               (unsigned)s_task_table[i].uxCurrentPriority,
               (unsigned)s_task_table[i].usStackHighWaterMark);
    }
}

static void _diag_task(void *argument)
{
    (void)argument;

    /* 首次多等一会儿：让各任务都跑过它们最长的那条路径 */
    osDelay(APP_DIAG_INTERVAL_S * 1000U);

    for (;;) {
        _diag_dump();
        osDelay(APP_DIAG_INTERVAL_S * 1000U);
    }
}

static void _app_diag_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "app_diag",
        .stack_size = 256 * 4,
        .priority   = osPriorityLow, /* 低优先级：不扰动实时路径 */
    };
    pl_task_new(_diag_task, NULL, &attr);
}
/* sw_post(4)：排在最后，让全部任务都已创建后再开始计时 */
sw_post_initcall(_app_diag_init);

#endif /* APP_DIAG_ENABLE */
