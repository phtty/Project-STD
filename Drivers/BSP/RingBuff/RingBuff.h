#pragma once

#include "main.h"

#define BUFFER_SIZE 2048 // 必须是2的整数次幂，便于优化

// 环形缓冲区数据结构
typedef struct RB_Struct {
    uint8_t data[BUFFER_SIZE];
    volatile uint16_t read_index;  // 读指针
    volatile uint16_t write_index; // 写指针
} RingBuff_t;

extern RingBuff_t xProtocol_RB;

uint8_t BSP_RB_IsEmpty(RingBuff_t *fifo);
uint8_t BSP_RB_IsFull(RingBuff_t *fifo);
uint16_t BSP_RB_GetAvailable(RingBuff_t *fifo);
uint16_t BSP_RB_GetFreeSpace(RingBuff_t *fifo);
uint8_t BSP_RB_FreeBuff(RingBuff_t *fifo);
uint8_t BSP_RB_PutByte(RingBuff_t *fifo, uint8_t byte);
uint16_t BSP_RB_PutByte_Bulk(RingBuff_t *fifo, const uint8_t *data, uint16_t len);
uint8_t BSP_RB_GetByte(RingBuff_t *fifo, uint8_t *byte);
uint16_t BSP_RB_GetByte_Bulk(RingBuff_t *fifo, uint8_t *data, uint16_t len);
uint8_t BSP_RB_PeekByte(RingBuff_t *fifo, uint16_t offset, uint8_t *byte);
uint16_t BSP_RB_PeekBlock(RingBuff_t *fifo, uint16_t offset, uint8_t *dest, uint16_t len);
uint16_t BSP_RB_GetContiguousLength(RingBuff_t *fifo, uint16_t offset);
uint16_t BSP_RB_SkipBytes(RingBuff_t *fifo, uint16_t len);
