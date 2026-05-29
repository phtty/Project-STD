/**
 * @file    pl_rtt.c
 * @brief   SEGGER RTT 调试输出初始化
 */

#include "SEGGER_RTT.h"
#include "initcall.h"

void pl_rtt_init(void)
{
    SEGGER_RTT_Init();
}
hw_pl_initcall(pl_rtt_init);
