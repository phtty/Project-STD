#ifndef LWIP_APP_TCP_SEVER_H
#define LWIP_APP_TCP_SEVER_H

#include "main.h"
#include "cmsis_os2.h"
#include "RingBuffer.h"
#include <stdio.h>
#include <stdbool.h>

#define TCP_SERVER_PORT  1145 // 服务器监听端口
#define RECV_BUFFER_SIZE 1024 // 接收缓冲区大小

extern RingBuffer Serial_RingBuff;
extern osThreadId_t tcpServerTaskHandle;
extern const osThreadAttr_t tcpServerTask_attributes;

void tcpServerTask(void *argument);

#endif // !LWIP_APP_TCP_SEVER_H