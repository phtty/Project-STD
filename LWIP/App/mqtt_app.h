#pragma once

#include "main.h"
#include "cmsis_os2.h"
#include "RingBuffer.h"

typedef enum {
    no_connect,
    connecting,
    connected,
} mqtt_StateMachine;

extern uint8_t mqtt_rcv_buf[];
extern mqtt_StateMachine mqtt_state;

void mqtt_connection(void);
void mqtt_send_data(const char *topic, const char *message);
