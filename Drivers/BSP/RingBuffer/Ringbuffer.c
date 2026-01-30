#include "RingBuffer.h"

/**
 * @brief 检查缓冲区是否为空
 * @param fifo 指向环形缓冲区的指针
 * @retval 1 缓冲区为空
 * @retval 0 缓冲区非空
 */
uint8_t BSP_RB_IsEmpty(RingBuffer *fifo)
{
    return fifo->read_index == fifo->write_index;
}

/**
 * @brief 检查缓冲区是否已满
 * @param fifo 指向环形缓冲区的指针
 * @retval 1 缓冲区已满
 * @retval 0 缓冲区未满
 */
uint8_t BSP_RB_IsFull(RingBuffer *fifo)
{
    return ((fifo->write_index + 1) & (BUFFER_SIZE - 1)) == fifo->read_index;
}

/**
 * @brief 获取缓冲区中可读数据量
 * @param fifo 指向环形缓冲区的指针
 * @return 当前可读取的字节数
 */
uint16_t BSP_RB_GetAvailable(RingBuffer *fifo)
{
    return (fifo->write_index - fifo->read_index) & (BUFFER_SIZE - 1);
}

/**
 * @brief 获取缓冲区剩余空间
 * @param fifo 指向环形缓冲区的指针
 * @return 当前可写入的字节数
 */
uint16_t BSP_RB_GetFreeSpace(RingBuffer *fifo)
{
    return BUFFER_SIZE - BSP_RB_GetAvailable(fifo) - 1;
}

/**
 * @brief 清空环形缓冲区
 *
 * @param fifo 指向环形缓冲区的指针
 * @retval 1 写入成功
 * @retval 0 缓冲区已满，写入失败
 */
uint8_t BSP_RB_FreeBuff(RingBuffer *fifo)
{
    fifo->read_index = fifo->write_index;
    return 1;
}

/**
 * @brief 写入单个字节到缓冲区
 * @param fifo 指向环形缓冲区的指针
 * @param byte 要写入的字节
 * @retval 1 写入成功
 * @retval 0 缓冲区已满，写入失败
 */
uint8_t BSP_RB_PutByte(RingBuffer *fifo, uint8_t byte)
{
    if (BSP_RB_IsFull(fifo)) {
        return 0; // 缓冲区满，写入失败
    }

    fifo->data[fifo->write_index] = byte;
    fifo->write_index             = (fifo->write_index + 1) & (BUFFER_SIZE - 1);
    return 1; // 写入成功
}

/**
 * @brief 批量写入多个字节到缓冲区
 * @param fifo 指向环形缓冲区的指针
 * @param data 要写入的数据指针
 * @param len 要写入的字节数
 * @return 实际写入的字节数
 */
uint16_t BSP_RB_PutByte_Bulk(RingBuffer *fifo, const uint8_t *data, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++) {
        if (!BSP_RB_PutByte(fifo, data[i])) {
            break; // 缓冲区满，停止写入
        }
    }

    return i; // 返回实际写入的字节数
}

/**
 * @brief 从缓冲区读取单个字节
 * @param fifo 指向环形缓冲区的指针
 * @param byte 存储读取结果的指针
 * @retval 1 读取成功
 * @retval 0 缓冲区为空，读取失败
 */
uint8_t BSP_RB_GetByte(RingBuffer *fifo, uint8_t *byte)
{
    if (BSP_RB_IsEmpty(fifo)) {
        return 0; // 缓冲区空，读取失败
    }

    *byte            = fifo->data[fifo->read_index];
    fifo->read_index = (fifo->read_index + 1) & (BUFFER_SIZE - 1);
    return 1; // 读取成功
}

/**
 * @brief 批量读取多个字节
 * @param fifo 指向环形缓冲区的指针
 * @param data 存储读取结果的指针
 * @param len 要读取的字节数
 * @return 实际读取的字节数
 */
uint16_t BSP_RB_GetByte_Bulk(RingBuffer *fifo, uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        if (!BSP_RB_GetByte(fifo, &data[i])) {
            break; // 缓冲区空，停止读取
        }
    }
    return i; // 返回实际读取的字节数
}

/**
 * @brief 窥视缓冲区中的单个字节（不移动读指针）
 * @param fifo 指向环形缓冲区的指针
 * @param offset 从当前读指针开始的偏移量
 * @param byte 存储读取结果的指针
 * @retval 1 读取成功
 * @retval 0 偏移超出有效范围
 */
uint8_t BSP_RB_PeekByte(RingBuffer *fifo, uint16_t offset, uint8_t *byte)
{
    uint16_t avail = BSP_RB_GetAvailable(fifo);

    if (offset >= avail)
        return 0; // 偏移超出有效范围

    uint16_t physical_index = (fifo->read_index + offset) & (BUFFER_SIZE - 1);
    *byte                   = fifo->data[physical_index];
    return 1;
}

/**
 * @brief 窥视缓冲区中的数据块（不移动读指针）
 * @param fifo 指向环形缓冲区的指针
 * @param offset 从当前读指针开始的偏移量
 * @param dest 存储读取结果的目标缓冲区
 * @param len 要读取的字节数
 * @return 实际读取的字节数
 */
uint16_t BSP_RB_PeekBlock(RingBuffer *fifo, uint16_t offset, uint8_t *dest, uint16_t len)
{
    int16_t avail = BSP_RB_GetAvailable(fifo) - offset;
    if (avail <= 0)
        return 0;

    len = (len > avail) ? avail : len; // 限制长度

    // 计算物理位置
    uint16_t start_index = (fifo->read_index + offset) & (BUFFER_SIZE - 1);
    uint16_t contiguous  = BUFFER_SIZE - start_index;

    if (len <= contiguous) {
        // 单段复制
        memcpy(dest, &fifo->data[start_index], len);
    } else {
        // 两段复制（处理回绕）
        memcpy(dest, &fifo->data[start_index], contiguous);
        memcpy(dest + contiguous, fifo->data, len - contiguous);
    }
    return len;
}

/**
 * @brief 获取从指定偏移开始的连续数据长度，一般用于判断是否存在回绕
 * @param fifo 指向环形缓冲区的指针
 * @param offset 从当前读指针开始的偏移量
 * @return 从偏移位置开始的连续字节数
 */
uint16_t BSP_RB_GetContiguousLength(RingBuffer *fifo, uint16_t offset)
{
    uint16_t avail = BSP_RB_GetAvailable(fifo);

    if (offset >= avail) return 0;

    uint16_t start_index = (fifo->read_index + offset) & (BUFFER_SIZE - 1);
    uint16_t contiguous  = BUFFER_SIZE - start_index;
    uint16_t remaining   = avail - offset;

    return (contiguous < remaining) ? contiguous : remaining;
}

/**
 * @brief 跳过指定字节数（移动读指针）
 * @param fifo 指向环形缓冲区的指针
 * @param len 要跳过的字节数
 * @return 实际跳过的字节数
 */
uint16_t BSP_RB_SkipBytes(RingBuffer *fifo, uint16_t len)
{
    uint16_t avail = BSP_RB_GetAvailable(fifo);
    len            = (len > avail) ? avail : len; // 限制长度

    fifo->read_index = (fifo->read_index + len) & (BUFFER_SIZE - 1);
    return len;
}
