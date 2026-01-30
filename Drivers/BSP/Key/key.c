#include "key.h"
#include "display.h"
#include "gpio.h"

bool key_reflash_flag = false;
// KeyStatus key_flag         = key_none;

void handle_key(void)
{
    if (key_reflash_flag == false)
        return;
    key_reflash_flag = false;

    // osDelay(5);

    if (GPIO_PIN_RESET == HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_10)) {

    } else if (GPIO_PIN_RESET == HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_11)) {

    } else if (GPIO_PIN_RESET == HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_12)) {
    }
}