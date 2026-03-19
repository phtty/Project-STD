#include "IOCtrl.h"
#include "cmsis_os2.h"

extern osEventFlagsId_t SW123_Event;

/*
 * @brief  BSP_HSCtrl  (黄闪控制)
 * @param  HSEnable: true: 打开, false: 关闭
 */
void BSP_HSCtrl(bool HSEnable)
{
    do {
        HAL_GPIO_WritePin(Lane_Light_GPIO_Port, Lane_Light_Pin, HSEnable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    } while (0);
}

/*
 * @brief  BSP_LILCtrl  (车道灯控制)
 * @param  LILEnable: true: 绿灯, false: 红灯
 */
void BSP_LILCtrl(bool LILEnable)
{
    do {
        HAL_GPIO_WritePin(Flash_Light_GPIO_Port, Flash_Light_Pin, LILEnable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    } while (0);
}

__weak void Sw1Ctrl(void)
{
}

__weak void Sw2Ctrl(void)
{
}

__weak void Sw3Ctrl(void)
{
}

void BSP_Sw123_Task(void *argument)
{
    uint32_t flags = 0;
    SW123_Event    = osEventFlagsNew(NULL);

    for (;;) {
        flags = osEventFlagsWait(
            SW123_Event,
            SW1_EVENT | SW2_EVENT | SW3_EVENT,
            osFlagsWaitAny,
            200);

        switch (flags) {
            case SW1_EVENT:
                Sw1Ctrl();
                break;
            case SW2_EVENT:
                Sw2Ctrl();
                break;
            case SW3_EVENT:
                Sw3Ctrl();
                break;
            default:
                break;
        }

        osDelay(50);
    }
}