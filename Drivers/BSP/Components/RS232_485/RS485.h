#pragma once

#include "main.h"

#include "usart.h"
#include "cmsis_os2.h"

#define RS485_BUF_SIZE (2048U)

extern uint8_t rs485_RxBuf[RS485_BUF_SIZE];
extern osThreadId_t rs485ManageTaskHandle;
extern const osThreadAttr_t rs485ManageTask_attributes;

void rs485ManageTask(void *argument);
void RS485_IdleHandle(void);
