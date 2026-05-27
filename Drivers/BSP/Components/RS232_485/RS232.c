#include "RS232.h"

#include "protocol.h"

static UART_HandleTypeDef *rs232_1 = &huart3;
static UART_HandleTypeDef *rs232_2 = &huart6;
uint8_t rs232_1_RxBuf[RS232_1_BUF_SIZE];
uint8_t rs232_2_RxBuf[RS232_2_BUF_SIZE];

// 定义管理rs232_1连接的任务句柄
osThreadId_t rs2321ManageTaskHandle;
const osThreadAttr_t rs2321ManageTask_attributes = {
    .name       = "rs2321ManageTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
// 定义管理rs232_2连接的任务句柄
osThreadId_t rs2322ManageTaskHandle;
const osThreadAttr_t rs2322ManageTask_attributes = {
    .name       = "rs2322ManageTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};
osMessageQueueId_t rs232_1_RxQueue;
osMessageQueueId_t rs232_2_RxQueue;

/**
 * @brief rs232_1连接管理任务
 *
 * @param argument
 */
void rs2321ManageTask(void *argument)
{
    ch_meta_t meta = {
        .type              = CH_TYPE_UART,
        .protocol          = PROTO_MASK_NONE,
        .handle.uart.huart = rs232_1,
    };
    rs232_1_RxQueue = osMessageQueueNew(1, sizeof(uint16_t), NULL);

    HAL_UART_Receive_DMA(rs232_1, rs232_1_RxBuf, RS232_1_BUF_SIZE);
    __HAL_UART_ENABLE_IT(rs232_1, UART_IT_IDLE);

    for (;;) {
        uint16_t rs232_1_RxLen = 0;
        osMessageQueueGet(rs232_1_RxQueue, &rs232_1_RxLen, 0, osWaitForever);

        // 临界保护区，防止多个信道同时写ringbuff
        osMutexAcquire(g_ringbuf_mutex, osWaitForever);

        BSP_RB_PutByte_Bulk(&g_iap_ringbuf, rs232_1_RxBuf, rs232_1_RxLen);

        // 元数据入队
        osMessageQueuePut(g_meta_queue, &meta, 0, 100);

        osMutexRelease(g_ringbuf_mutex);
    }
}

void RS232_1_IdleHandle(void)
{
    HAL_UART_DMAStop(rs232_1);

    uint16_t len = RS232_1_BUF_SIZE - __HAL_DMA_GET_COUNTER(rs232_1->hdmarx);
    osMessageQueuePut(rs232_1_RxQueue, &len, 0, 0);

    HAL_UART_Receive_DMA(rs232_1, rs232_1_RxBuf, RS232_1_BUF_SIZE);
}

/**
 * @brief rs232_2连接管理任务
 *
 * @param argument
 */
void rs2322ManageTask(void *argument)
{
    ch_meta_t meta = {
        .type              = CH_TYPE_UART,
        .protocol          = PROTO_MASK_NONE,
        .handle.uart.huart = rs232_2,
    };
    rs232_2_RxQueue = osMessageQueueNew(1, sizeof(uint16_t), NULL);

    HAL_UART_Receive_DMA(rs232_2, rs232_2_RxBuf, RS232_2_BUF_SIZE);
    __HAL_UART_ENABLE_IT(rs232_2, UART_IT_IDLE);

    for (;;) {
        uint16_t rs232_2_RxLen = 0;
        osMessageQueueGet(rs232_2_RxQueue, &rs232_2_RxLen, 0, osWaitForever);

        // 临界保护区，防止多个信道同时写ringbuff
        osMutexAcquire(g_ringbuf_mutex, osWaitForever);

        BSP_RB_PutByte_Bulk(&g_iap_ringbuf, rs232_2_RxBuf, rs232_2_RxLen);

        // 元数据入队
        osMessageQueuePut(g_meta_queue, &meta, 0, 100);

        osMutexRelease(g_ringbuf_mutex);
    }
}

void RS232_2_IdleHandle(void)
{
    HAL_UART_DMAStop(rs232_2);

    uint16_t len = RS232_2_BUF_SIZE - __HAL_DMA_GET_COUNTER(rs232_2->hdmarx);
    osMessageQueuePut(rs232_2_RxQueue, &len, 0, 0);

    HAL_UART_Receive_DMA(rs232_2, rs232_2_RxBuf, RS232_2_BUF_SIZE);
}
