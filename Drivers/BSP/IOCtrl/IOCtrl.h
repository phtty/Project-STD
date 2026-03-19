#ifndef __IOCTRL_H
#define __IOCTRL_H

#include "main.h"

enum {
    SW1_EVENT = 0,
    SW2_EVENT,
    SW3_EVENT,
};

__weak void Sw1Ctrl(void);
__weak void Sw2Ctrl(void);
__weak void Sw3Ctrl(void);
void BSP_Sw123_Task(void *argument);

#endif