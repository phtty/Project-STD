/**
 * @file        pl_rtc.c
 * @brief       RTC 抽象（hw_pl_initcall 优先级 3）
 */

#include "pl_rtc.h"
#include "rtc.h"
#include "initcall.h"

void pl_rtc_init(void)
{
    MX_RTC_Init();
}
hw_pl_initcall(pl_rtc_init);

pl_rtc_handle_t pl_rtc_get_handle(void)
{
    return (pl_rtc_handle_t)&hrtc;
}

bool pl_rtc_bkup_write(pl_rtc_handle_t h, uint32_t reg, uint32_t value)
{
    HAL_RTCEx_BKUPWrite((RTC_HandleTypeDef *)h, reg, value);
    return true;
}

uint32_t pl_rtc_bkup_read(pl_rtc_handle_t h, uint32_t reg)
{
    return HAL_RTCEx_BKUPRead((RTC_HandleTypeDef *)h, reg);
}

uint32_t pl_rtc_get_timestamp(pl_rtc_handle_t h)
{
    (void)h;
    return 0; /* TODO: 实现 Unix 时间戳读取 */
}

bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts)
{
    (void)h;
    (void)ts;
    return false; /* TODO: 实现 Unix 时间戳设置 */
}
