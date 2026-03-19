#include "key.h"
#include "display.h"
#include "gpio.h"
#include "freertos_os2.h"
#include "render.h"

extern osSemaphoreId_t test_semaphore;

static const uint8_t testDisBuf[14][120] = {
    "重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重",
    "庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆",
    "创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创",
    "迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪",
    "科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科",
    "技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技",
    "贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵贵",
    "州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州州",
    "治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治治",
    "超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超超",
    "屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏",
    "测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测",
    "试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试",
    "                                                                                                                        ",
};

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY_TST_Pin) {
        osSemaphoreRelease(test_semaphore);
    }
}

void test_Task(void *argument)
{
    uint32_t test_cnt       = 0;
    uint32_t testDisPlayCnt = 0;
    DispColor_t tempColor   = red;
    FontSize_t tempSize     = font_14;
    FontType_t tempType     = font_fs;
    while (1) {
        osSemaphoreWait(test_semaphore, osWaitForever);

        test_cnt++;

        switch (test_cnt) {
            case 1:
                Disp_Fill(green);
                break;
            case 2:
                Disp_Fill(red);
                break;
            case 3:
                Disp_Fill(yellow);
                break;
            case 4:
                goto LCD_Test
                break;
            default:
                break;
        }
    }

LCD_Test:
    while (1) {
        if (osSemaphoreWait(test_semaphore, 0) == osOK)
            NVIC_SystemReset();

        RenderString(0, 0, (uint8_t *)testDisBuf[testDisPlayCnt], 120, tempColor, tempSize, tempType);
        testDisPlayCnt++;
        if (testDisPlayCnt >= 14) {
            tempColor++;
            tempSize++;
            tempType++;
            if (tempSize >= font_32) {
                tempSize = font_14;
            }
            if (tempColor >= yellow) {
                tempColor = red;
            }
            if (tempType >= font_st) {
                tempType = font_fs;
            }
            testDisPlayCnt = 0;
        }

        osDelay(1000);
    }
}