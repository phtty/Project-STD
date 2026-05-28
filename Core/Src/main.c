/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : 主入口
 ******************************************************************************
 */

#include "main.h"
#include "initcall.h"
#include "pl_sys.h"

void app_boot(void);

int main(void)
{
    __enable_irq();

    HAL_Init();
    SystemClock_Config();

    initcall_run(__hw_initcall_start, __hw_initcall_end);

    app_boot();

    for (;;) {}
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
