#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

#include <string.h>
#include <stdint.h>

#define BUFFER_SIZE 2048 // 必须是2的幂，便于优化

// 环形缓冲区数据结构
typedef struct RB_Struct {
    uint8_t data[BUFFER_SIZE];
    volatile uint16_t read_index;  // 读指针
    volatile uint16_t write_index; // 写指针
} RingBuffer;

uint8_t BSP_RB_IsEmpty(RingBuffer *fifo);
uint8_t BSP_RB_IsFull(RingBuffer *fifo);
uint16_t BSP_RB_GetAvailable(RingBuffer *fifo);
uint16_t BSP_RB_GetFreeSpace(RingBuffer *fifo);
uint8_t BSP_RB_FreeBuff(RingBuffer *fifo);
uint8_t BSP_RB_PutByte(RingBuffer *fifo, uint8_t byte);
uint16_t BSP_RB_PutByte_Bulk(RingBuffer *fifo, const uint8_t *data, uint16_t len);
uint8_t BSP_RB_GetByte(RingBuffer *fifo, uint8_t *byte);
uint16_t BSP_RB_GetByte_Bulk(RingBuffer *fifo, uint8_t *data, uint16_t len);
uint8_t BSP_RB_PeekByte(RingBuffer *fifo, uint16_t offset, uint8_t *byte);
uint16_t BSP_RB_PeekBlock(RingBuffer *fifo, uint16_t offset, uint8_t *dest, uint16_t len);
uint16_t BSP_RB_GetContiguousLength(RingBuffer *fifo, uint16_t offset);
uint16_t BSP_RB_SkipBytes(RingBuffer *fifo, uint16_t len);

#endif // !__RINGBUFFER_H__