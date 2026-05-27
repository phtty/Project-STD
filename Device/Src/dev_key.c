#include "main.h"
#include "cmsis_os2.h"
/**
 * @file    dev_key.c
 * @brief   按键/拨码开关设备实现
 */

#include "dev_key.h"
#include <stddef.h>

static dev_key_cb_t s_key_cb[8];

void dev_key_init(void)
{
    for (int i = 0; i < 8; i++) s_key_cb[i] = NULL;
}

void dev_key_register_cb(uint8_t key_id, dev_key_cb_t cb)
{
    if (key_id < 8) s_key_cb[key_id] = cb;
}

/* ---- EXTI ISR (从 stm32f4xx_it.c 迁移) ---- */
extern osSemaphoreId_t  test_semaphore;
extern osEventFlagsId_t SW123_Event;

void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(KEY_TST_Pin);
}
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(SW3_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW2_Pin);
    HAL_GPIO_EXTI_IRQHandler(SW1_Pin);
}
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY_TST_Pin)
        osSemaphoreRelease(test_semaphore);
    if (GPIO_Pin == SW1_Pin)
        osEventFlagsSet(SW123_Event, SW1_EVENT);
    if (GPIO_Pin == SW2_Pin)
        osEventFlagsSet(SW123_Event, SW2_EVENT);
    if (GPIO_Pin == SW3_Pin)
        osEventFlagsSet(SW123_Event, SW3_EVENT);
}
