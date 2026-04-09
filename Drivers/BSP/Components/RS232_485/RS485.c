#include "RS485.h"

#include "gpio.h"
#include "protocol.h"

static UART_HandleTypeDef *rs485 = &huart1;
uint8_t rs485_RxBuf[RS485_BUF_SIZE];

// 定义管理rs485连接的任务句柄
osThreadId_t rs485ManageTaskHandle;
const osThreadAttr_t rs485ManageTask_attributes = {
    .name       = "rs485ManageTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
osMessageQueueId_t rs485_RxQueue;

/**
 * @brief rs485连接管理任务
 *
 * @param argument
 */
void rs485ManageTask(void *argument)
{
    ch_meta_t meta = {
        .type              = CH_TYPE_UART,
        .protocol          = PROTO_MASK_IAP,
        .handle.uart.huart = rs485,
    };
    rs485_RxQueue = osMessageQueueNew(1, sizeof(uint16_t), NULL);

    RS485_RE = 0; // 设置为接收
    HAL_UART_Receive_DMA(rs485, rs485_RxBuf, RS485_BUF_SIZE);
    __HAL_UART_ENABLE_IT(rs485, UART_IT_IDLE);

    for (;;) {
        uint16_t rs485_RxLen = 0;
        osMessageQueueGet(rs485_RxQueue, &rs485_RxLen, 0, osWaitForever);

        // 临界保护区，防止多个信道同时写ringbuff
        osMutexAcquire(g_ringbuf_mutex, osWaitForever);

        BSP_RB_PutByte_Bulk(&g_iap_ringbuf, rs485_RxBuf, rs485_RxLen);

        // 元数据入队
        osMessageQueuePut(g_meta_queue, &meta, 0, 100);

        osMutexRelease(g_ringbuf_mutex);
    }
}

void RS485_IdleHandle(void)
{
    HAL_UART_DMAStop(rs485);

    uint16_t len = RS485_BUF_SIZE - __HAL_DMA_GET_COUNTER(rs485->hdmarx);
    osMessageQueuePut(rs485_RxQueue, &len, 0, 0);

    HAL_UART_Receive_DMA(rs485, rs485_RxBuf, RS485_BUF_SIZE);
}
