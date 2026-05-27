/**
 * @file    pl_sys.c
 * @brief   系统基础实现：时钟、复位、HAL MSP 初始化
 *
 * 合并自 pl_clock.c / pl_system.c / pl_msp.c
 */

#include "pl_sys.h"
#include "main.h"

/* ================================================================
 *  系统时钟配置：HSE → PLL → 168MHz
 *  由 main.c 在 HAL_Init 之后手动调用（非 initcall）
 * ================================================================ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.LSIState       = RCC_LSI_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 4;
    osc.PLL.PLLN       = 168;
    osc.PLL.PLLP       = RCC_PLLP_DIV2;
    osc.PLL.PLLQ       = 4;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
        Error_Handler();

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
        Error_Handler();
}

/* ================================================================
 *  系统复位
 * ================================================================ */

void pl_system_reset(void)
{
    NVIC_SystemReset();
}

/* ================================================================
 *  HAL 全局 MSP 初始化（HAL_Init 回调）
 * ================================================================ */

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* FreeRTOS 依赖 PendSV 做上下文切换，必须设为最低优先级 */
    HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);
}
