#pragma once

#include "main.h"

#include "lwip.h"
#include "cmsis_os2.h"

#include "RingBuff.h"

#define UDP_PORT (10011U)

typedef struct {
    struct netconn *conn;
    int16_t channel_index;
} udp_conn_arg_t;

extern osThreadId_t udpManageTaskHandle;
extern const osThreadAttr_t udpManageTask_attributes;

void udpManageTask(void *argument);
