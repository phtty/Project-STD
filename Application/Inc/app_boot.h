/**
 * @file    app_boot.h
 * @brief   系统启动编排器
 */

#pragma once

/** @brief 系统启动编排入口：按序启动各层与外设，进入主循环前调用一次 */
void app_boot(void);
