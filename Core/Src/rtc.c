/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    rtc.c
 * @brief   This file provides code for the configuration
 *          of the RTC instances.
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

    /* USER CODE BEGIN RTC_Init 0 */

    /* USER CODE END RTC_Init 0 */

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    /* USER CODE BEGIN RTC_Init 1 */

    /* USER CODE END RTC_Init 1 */

    /** Initialize RTC Only
     */
    hrtc.Instance            = RTC;
    hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv   = 127;
    hrtc.Init.SynchPrediv    = 255;
    hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
    if (HAL_RTC_Init(&hrtc) != HAL_OK) {
        Error_Handler();
    }

    /* USER CODE BEGIN Check_RTC_BKUP */

    /* USER CODE END Check_RTC_BKUP */

    /** Initialize RTC and set the Time and Date
     */
    sTime.Hours          = 0x0;
    sTime.Minutes        = 0x0;
    sTime.Seconds        = 0x0;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
        Error_Handler();
    }
    sDate.WeekDay = RTC_WEEKDAY_MONDAY;
    sDate.Month   = RTC_MONTH_JANUARY;
    sDate.Date    = 0x1;
    sDate.Year    = 0x0;

    if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
        Error_Handler();
    }
    /* USER CODE BEGIN RTC_Init 2 */

    /* USER CODE END RTC_Init 2 */
}

void HAL_RTC_MspInit(RTC_HandleTypeDef *rtcHandle)
{

    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
    if (rtcHandle->Instance == RTC) {
        /* USER CODE BEGIN RTC_MspInit 0 */

        /* USER CODE END RTC_MspInit 0 */

        /** Initializes the peripherals clock
         */
        PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        PeriphClkInitStruct.RTCClockSelection    = RCC_RTCCLKSOURCE_LSI;
        if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) {
            Error_Handler();
        }

        /* RTC clock enable */
        __HAL_RCC_RTC_ENABLE();
        /* USER CODE BEGIN RTC_MspInit 1 */

        /* USER CODE END RTC_MspInit 1 */
    }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef *rtcHandle)
{

    if (rtcHandle->Instance == RTC) {
        /* USER CODE BEGIN RTC_MspDeInit 0 */

        /* USER CODE END RTC_MspDeInit 0 */
        /* Peripheral clock disable */
        __HAL_RCC_RTC_DISABLE();
        /* USER CODE BEGIN RTC_MspDeInit 1 */

        /* USER CODE END RTC_MspDeInit 1 */
    }
}

/* USER CODE BEGIN 1 */
time_t RTC_GetUnixTimestamp(void)
{
    RTC_DateTypeDef sDate;
    RTC_TimeTypeDef sTime;
    struct tm timeStruct = {0};

    // 从 RTC 硬件读取当前时间和日期
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

    // 将 RTC 数据结构 (2000+年份) 转换为 tm 结构体 (Unix 时间戳基准从 1900 年开始)
    timeStruct.tm_year = (sDate.Year + 2000) - 1900; // sDate.Year 通常为 0-99
    timeStruct.tm_mon  = sDate.Month - 1;            // tm_mon 范围 0-11
    timeStruct.tm_mday = sDate.Date;
    timeStruct.tm_hour = sTime.Hours;
    timeStruct.tm_min  = sTime.Minutes;
    timeStruct.tm_sec  = sTime.Seconds;

    // 将 tm 结构体转换为 Unix 时间戳（默认为 UTC 时间）
    time_t utc_timestamp = mktime(&timeStruct);
    return utc_timestamp;
}

/**
 * @brief 使用 Unix 时间戳设置 RTC 时间
 * @param utc_timestamp 要设置的 Unix 时间戳（从 1970-01-01 00:00:00 UTC 开始的秒数）
 * @retval HAL 状态
 */
HAL_StatusTypeDef RTC_Set_UnixTimeStamp(time_t utc_timestamp)
{
    struct tm *timeinfo;
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    // 将 Unix 时间戳转换为结构体时间/日期（使用 gmtime 得到 UTC 时间）
    // 接收到的对时指令本身已是 UTC 时间，无需时区转换
    timeinfo = gmtime(&utc_timestamp);

    // 将 struct tm 转换为 RTC HAL 需要的结构体
    // 注意年份：struct tm 的 tm_year 是从 1900 年开始的，RTC 需要 2000 年基准偏移
    sDate.Year    = (timeinfo->tm_year + 1900) - 2000;
    sDate.Month   = timeinfo->tm_mon + 1; // tm_mon 范围 0~11，RTC 要求 1~12
    sDate.Date    = timeinfo->tm_mday;
    sDate.WeekDay = timeinfo->tm_wday; // 可选，RTC 可自动计算

    sTime.Hours   = timeinfo->tm_hour;
    sTime.Minutes = timeinfo->tm_min;
    sTime.Seconds = timeinfo->tm_sec;
    // 补充一些通常需要的参数
    sTime.TimeFormat     = RTC_HOURFORMAT_24;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    // RTC 已在 MX_RTC_Init 中完成初始化，无需 DeInit。
    // HAL_RTC_SetDate 内部进入 INIT 模式，HAL_RTC_SetTime 内部退出 INIT 模式。
    // DeInit 会关闭 RTC 时钟导致后续写寄存器超时（约 3 秒延迟）。

    // 先写日期，再写时间（HAL 要求此顺序，两者配对完成 INIT 模式的进入与退出）
    if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
        return HAL_ERROR;
    }

    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
        return HAL_ERROR;
    }

    return HAL_OK;
}
/* USER CODE END 1 */
