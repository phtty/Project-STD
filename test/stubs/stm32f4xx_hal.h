/**
 * @file    stm32f4xx_hal.h
 * @brief   HAL 的 host 替身 —— 只够编 pl_tim.c / pl_uart.c
 *
 * 存在的理由只有一个：**验证 ISR 在 initcall 跑之前也是安全的**。
 *
 * 真 HAL 头会拉进 CMSIS 内核寄存器定义与整套外设结构体，host 编不了；而这里需要的
 * 只是"让 ISR 里的那次 HAL_xxx_IRQHandler 调用可观测"。所有 HAL_* 实现在测试文件里
 * （记调用次数），本头只给类型与宏的形状。
 *
 * 放在 test/stubs/ 下是安全的：实测没有任何被测 TU 直接或间接 include 真 HAL 头，
 * 不会遮蔽掉谁。（-I test/stubs 排在最前，一旦有 TU 需要真头就会当场编译失败，
 * 属可接受的失败方式。）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h> /* NULL —— pl_tim.c 用它 */
#include <stdint.h>

/* ---- 类型形状 ---- */

typedef struct {
    uint32_t dummy;
} TIM_TypeDef;

typedef struct {
    uint32_t dummy;
} USART_TypeDef;

typedef struct {
    uint32_t dummy;
} DMA_HandleTypeDef;

typedef struct {
    TIM_TypeDef *Instance;
} TIM_HandleTypeDef;

typedef struct {
    USART_TypeDef     *Instance;
    DMA_HandleTypeDef *hdmarx;
} UART_HandleTypeDef;

typedef enum {
    HAL_OK   = 0x00,
    HAL_ERROR = 0x01,
    HAL_BUSY = 0x02,
    HAL_TIMEOUT = 0x03,
} HAL_StatusTypeDef;

typedef enum {
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn = -13,
    SysTick_IRQn = -1,
} IRQn_Type;

/* 四个定时器实例的"地址"：只要彼此不同即可，ISR 只拿它做透传 */
extern TIM_TypeDef   s_fake_tim2, s_fake_tim3, s_fake_tim4, s_fake_tim5, s_fake_tim6,
                     s_fake_tim7;
extern USART_TypeDef s_fake_usart1;

#define TIM2 (&s_fake_tim2)
#define TIM3 (&s_fake_tim3)
#define TIM4 (&s_fake_tim4)
#define TIM5 (&s_fake_tim5)
#define TIM6 (&s_fake_tim6)
#define TIM7 (&s_fake_tim7)

/* ---- 被 pl_tim.c / pl_uart.c 用到的 HAL 接口（实现在测试文件里） ---- */
void HAL_TIM_IRQHandler(TIM_HandleTypeDef *htim);
void HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *htim);
void HAL_IncTick(void);

void          HAL_UART_IRQHandler(UART_HandleTypeDef *huart);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart, uint8_t *data, uint16_t len,
                                    uint32_t timeout);
HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef *huart, uint8_t *data, uint16_t len);
HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef *huart);

void HAL_DMA_IRQHandler(DMA_HandleTypeDef *hdma);

void NVIC_DisableIRQ(IRQn_Type irq);
void NVIC_EnableIRQ(IRQn_Type irq);

/* ---- 寄存器访问宏：替身里退化成读写一个公共影子，只为让代码编得过 ---- */
extern uint32_t s_fake_reg;

#define __HAL_DBGMCU_FREEZE_TIM2() ((void)0)
#define __HAL_DBGMCU_FREEZE_TIM3() ((void)0)
#define __HAL_DBGMCU_FREEZE_TIM4() ((void)0)
#define __HAL_DBGMCU_FREEZE_TIM5() ((void)0)
#define __HAL_DBGMCU_FREEZE_TIM6() ((void)0)
#define __HAL_DBGMCU_FREEZE_TIM7() ((void)0)

#define UART_FLAG_IDLE (1U << 0)
#define UART_IT_IDLE   (1U << 1)

#define __HAL_UART_GET_FLAG(h, f)      ((s_fake_reg & (f)) != 0)
#define __HAL_UART_CLEAR_IDLEFLAG(h)   ((void)(h))
#define __HAL_UART_ENABLE_IT(h, it)    ((void)(h), (void)(it))
#define __HAL_DMA_GET_COUNTER(h)       (0U)

#define __NOP() ((void)0)
