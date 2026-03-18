#pragma once

#include "main.h"
#include "adc.h"
#include "cmsis_os2.h"

#define MEAN_PARAMETER 8 // 均值参数，用于滤波

extern osMutexId_t light_mutex;

extern osThreadId_t autoAdjLightTaskHandle;
extern const osThreadAttr_t autoAdjLightTask_attributes;

void autoAdjLightTask(void *argument);
