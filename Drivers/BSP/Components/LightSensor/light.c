#include "Light.h"

#include "display.h"

osThreadId_t autoAdjLightTaskHandle;
const osThreadAttr_t autoAdjLightTask_attributes = {
    .name       = "autoAdjLightTask",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityLow,
};

uint8_t LightSensor_ReadAvg(void)
{
    uint32_t temp_val  = 0;
    uint8_t temp_light = 0;

    for (uint32_t i = 0; i < MEAN_PARAMETER; i++) {
        HAL_ADC_Start(&hadc1);
        HAL_ADC_PollForConversion(&hadc1, 100);
        temp_val += HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
    }

    temp_val /= MEAN_PARAMETER; // 8次采样取平均

    if (temp_val > 4000)
        temp_val = 4000;

    temp_light = (uint8_t)(temp_val / 500);

    if (temp_light < 1)
        temp_light = 1;

    return temp_light;
}

void LightSensor_Auto(void)
{
    static uint8_t old_light = 0;
    uint8_t new_light        = LightSensor_ReadAvg();

    if (new_light == old_light)
        light_level = new_light;

    old_light = new_light;
}

void autoAdjLightTask(void *argument)
{
    for (;;) {
        LightSensor_Auto();
        osDelay(1000);
    }
}
