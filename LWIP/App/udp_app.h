#pragma once

#include "main.h"

#include "lwip.h"
#include "cmsis_os2.h"

#include "RingBuff.h"

#define UDP_PORT (10011U)

extern osThreadId_t udpManageTaskHandle;
extern const osThreadAttr_t udpManageTask_attributes;

void udpManageTask(void *argument);
