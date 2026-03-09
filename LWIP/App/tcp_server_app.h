#pragma once

#include "main.h"
#include "cmsis_os2.h"
#include "RingBuff.h"

#define TCP_SERVER_PORT 1145 // ·þÎñÆ÷¼àÌý¶Ë¿Ú

extern osThreadId_t tcpServerTaskHandle;
extern const osThreadAttr_t tcpServerTask_attributes;

void tcpServerTask(void *argument);
