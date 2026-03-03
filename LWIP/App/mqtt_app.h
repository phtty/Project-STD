#pragma once

#include "main.h"
#include "cmsis_os2.h"
#include "RingBuffer.h"

typedef enum {
    no_connect,
    connecting,
    connected,
} mqtt_StateMachine;

extern volatile mqtt_StateMachine mqtt_state;
extern osThreadId_t mqttManageTaskHandle;
extern const osThreadAttr_t mqttManageTask_attributes;
extern osSemaphoreId_t mqttConnSemHandle;

void mqttManageTask(void *argument);
void mqtt_connection(void);
void mqtt_send_data(const char *topic, const char *message);
