/**
 * @file    crc_utils.h
 * @brief   软件 CRC 算法工具（芯片无关的纯算法）
 *
 * 与 Platform/pl_crc.h 的区别：这里的函数是纯软件实现，不依赖任何硬件。
 * pl_crc 封装的是 STM32 硬件 CRC32 单元。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief   CRC-16/XMODEM 软件计算
 *
 * 多项式 0x8408，初始值 0xFFFF，结果取反。
 * 用于《车道设备接口规范》LDI 协议的帧校验。
 *
 * @param   data  数据指针
 * @param   len   数据长度（字节）
 * @return  CRC16 校验值
 */
uint16_t crc16_xmodem(const uint8_t *data, size_t len);

/**
 * @brief   CRC-16/IBM 软件计算
 *
 * 多项式 X^16+X^15+X^2+1 (0x8005)，初始值 0x0000。
 * 用于 Onbon BX-Y 系列 LED 控制卡协议的帧校验。
 *
 * @param   data  数据指针
 * @param   len   数据长度（字节）
 * @return  CRC16 校验值
 */
uint16_t crc16_ibm(const uint8_t *data, size_t len);

/**
 * @brief   CRC-32 软件计算
 *
 * 多项式 0xEDB88320 (Ethernet 标准)，初始值 0xFFFFFFFF，结果取反。
 * 与 STM32 硬件 CRC32 计算单元输出一致。
 *
 * @param   data  数据指针
 * @param   len   数据长度（字节）
 * @return  CRC32 校验值
 */
uint32_t crc32_calc(const uint8_t *data, size_t len);
