#include "key.h"
#include "display.h"
#include "gpio.h"
#include "freertos_os2.h"
#include "render.h"

extern osSemaphoreId_t test_semaphore;

static const uint8_t testDisBuf[][120] = {
    "重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重重",
    "庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆庆",
    "创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创创",
    "迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪迪",
    "科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科科",
    "技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技技",
    "屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏屏",
    "幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕幕",
    "测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测测",
    "试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试试",
    "                                                                                                                        ",
};

void test_Task(void *argument)
{
    uint32_t test_cnt       = 0;
    uint32_t testDisPlayCnt = 0;
    DispColor_t tempColor   = red;
    FontSize_t tempSize     = font_14;
    FontType_t tempType     = font_fs;

    for (;;) {
        osSemaphoreAcquire(test_semaphore, osWaitForever);

        test_cnt++;

        if (test_cnt == 1)
            Disp_Fill(green);
        else if (test_cnt == 2)
            Disp_Fill(red);
        else if (test_cnt == 3)
            Disp_Fill(yellow);
        else
            break;
    }

    for (;;) {
        if (osSemaphoreAcquire(test_semaphore, 0) == osOK)
            NVIC_SystemReset();

        RenderString(0, 0, (uint8_t *)testDisBuf[testDisPlayCnt], 120, tempColor, tempSize, tempType);
        testDisPlayCnt++;
        if (testDisPlayCnt >= 11) {
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