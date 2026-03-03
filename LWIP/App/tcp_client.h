#pragma once

#include "main.h"
#include "cmsis_os2.h"
#include "RingBuffer.h"

extern osThreadId_t tcpClientTaskHandle;
extern const osThreadAttr_t tcpClientTask_attributes;

void tcpClientTask(void *argument);
