/**
 * @file        pl_iwdg.c
 * @brief       独立看门狗抽象（hw_device_initcall 优先级 3）
 */

#include "pl_iwdg.h"
#include "iwdg.h"
#include "initcall.h"

void pl_iwdg_init(void)
{
    MX_IWDG_Init();
}
hw_device_initcall(pl_iwdg_init);

pl_iwdg_handle_t pl_iwdg_get_handle(void)
{
    return (pl_iwdg_handle_t)&hiwdg;
}

void pl_iwdg_refresh(pl_iwdg_handle_t h)
{
    HAL_IWDG_Refresh((IWDG_HandleTypeDef *)h);
}
