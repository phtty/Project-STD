#pragma once

#include "main.h"
#include "cmsis_os2.h"
#include "ring_buffer.h"

#define TCP_SERVER_PORT 1145 // 服务器监听端口

extern osThreadId_t tcpServerTaskHandle;
extern const osThreadAttr_t tcpServerTask_attributes;

void tcpServerTask(void *argument);
