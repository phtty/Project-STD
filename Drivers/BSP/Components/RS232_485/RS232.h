#pragma once

#include "main.h"

#include "usart.h"
#include "cmsis_os2.h"

#define RS232_1_BUF_SIZE (2048U)
#define RS232_2_BUF_SIZE (2048U)

extern uint8_t rs232_1_RxBuf[RS232_1_BUF_SIZE];
extern uint8_t rs232_2_RxBuf[RS232_2_BUF_SIZE];

extern osThreadId_t rs2321ManageTaskHandle;
extern const osThreadAttr_t rs2321ManageTask_attributes;
extern osThreadId_t rs2322ManageTaskHandle;
extern const osThreadAttr_t rs2322ManageTask_attributes;

void rs2321ManageTask(void *argument);
void rs2322ManageTask(void *argument);

void RS232_1_IdleHandle(void);
void RS232_2_IdleHandle(void);
