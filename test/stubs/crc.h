/**
 * @file    crc.h
 * @brief   CubeMX crc.h 的 host 替身 —— 只够编 Platform/Src/pl_crc.c
 *
 * 真的 boards/<板>/Core/Inc/crc.h 会 `#include "main.h"` 并声明 `hcrc`，
 * 而 CubeMX 的 main.h 会拉进 HAL 与整套 Core 配置，host 编不了。
 *
 * 这里只给 pl_crc.c 真正用到的两样：句柄类型与 `hcrc` 的声明。
 * 句柄与 MX_CRC_Init 的**实现**在测试文件里（test/test_crc.c），
 * 因为那个桩要模拟硬件 CRC 单元的状态机 —— 见该文件的说明。
 */

#pragma once

#include "stm32f4xx_hal.h"

extern CRC_HandleTypeDef hcrc;

void MX_CRC_Init(void);
