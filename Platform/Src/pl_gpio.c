/**
 * @file        pl_gpio.c
 * @brief       GPIO 初始化 + 引脚读写（Platform 层封装，隔离芯片地址）
 */

#include "pl_gpio.h"
#include "gpio.h"
#include "stm32f4xx_hal.h"
#include "initcall.h"

/* ---- 端口 ID → 芯片基地址映射（换芯片只改此表） ---- */
static const uint32_t g_port_base[] = {
    [PL_PORT_A] = (uint32_t)GPIOA,
    [PL_PORT_B] = (uint32_t)GPIOB,
    [PL_PORT_C] = (uint32_t)GPIOC,
    [PL_PORT_D] = (uint32_t)GPIOD,
    [PL_PORT_E] = (uint32_t)GPIOE,
    [PL_PORT_F] = (uint32_t)GPIOF,
    [PL_PORT_G] = (uint32_t)GPIOG,
    [PL_PORT_H] = (uint32_t)GPIOH,
};

void pl_gpio_init(void)
{
    MX_GPIO_Init();
}
hw_pl_initcall(pl_gpio_init);

void pl_gpio_write(pl_port_t port, uint8_t pin, bool high)
{
    if (port >= PL_PORT_MAX) return;
    GPIO_TypeDef *gpio = (GPIO_TypeDef *)g_port_base[port];
    uint16_t mask      = (uint16_t)(1U << pin);
    if (high)
        gpio->BSRR = mask;
    else
        gpio->BSRR = (uint32_t)mask << 16;
}

bool pl_gpio_read(pl_port_t port, uint8_t pin)
{
    if (port >= PL_PORT_MAX) return false;
    GPIO_TypeDef *gpio = (GPIO_TypeDef *)g_port_base[port];
    return (gpio->IDR & (1U << pin)) != 0;
}
