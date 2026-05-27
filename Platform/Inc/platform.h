/**
 * @file    platform.h
 * @brief   Platform 层聚合头文件
 *
 * 包含全部 pl_* 模块接口，上层只需 include 这一个头文件即可使用整个 Platform 层。
 */

#pragma once

#include "pl_sys.h"
#include "pl_gpio.h"
#include "pl_dma.h"
#include "pl_uart.h"
#include "pl_crc.h"
#include "pl_rtc.h"
#include "pl_iwdg.h"
#include "pl_tim.h"
#include "pl_eth.h"
#include "pl_net.h"
#include "pl_flash.h"
