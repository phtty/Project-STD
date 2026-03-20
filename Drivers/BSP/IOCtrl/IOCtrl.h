#ifndef __IOCTRL_H
#define __IOCTRL_H

#include "main.h"

enum {
    SW1_EVENT = 1,
    SW2_EVENT = 2,
    SW3_EVENT = 4,
};

void BSP_HSCtrl(bool HSEnable);
void BSP_LILCtrl(bool LILEnable);

__weak void Sw1Ctrl(void);
__weak void Sw2Ctrl(void);
__weak void Sw3Ctrl(void);
void BSP_Sw123_Task(void *argument);

#endif