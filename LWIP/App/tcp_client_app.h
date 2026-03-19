#pragma once

#include "main.h"

#include "cmsis_os2.h"
#include "RingBuff.h"

extern osThreadId_t tcpClientTaskHandle;
extern const osThreadAttr_t tcpClientTask_attributes;

void tcpClientTask(void *argument);
