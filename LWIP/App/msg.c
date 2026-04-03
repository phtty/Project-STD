#include "msg.h"

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

#include "iap.h"
#include "protocol.h"
#include "mqtt_app.h"

osMutexId_t gx_RingBufMutex;
osMessageQueueId_t gx_MDataQueue;
osThreadId_t MDataRouterHandle;
osMessageQueueId_t ParserQueue[32] = {0};
uint8_t ParserQueueCnt             = 0;

/**
 * @brief 创建数据收发使用的线程同步资源
 *
 */
void CH_Initialize(void)
{
    osMutexAttr_t rb_mutex = {
        .name      = "gx_RingBufMutex",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0,
    };
    gx_RingBufMutex = osMutexNew(&rb_mutex);

    // 接收任务向元数据路由任务发送的队列
    osMessageQueueAttr_t mdata_queue = {
        .name = "gx_MDataQueue",
    };
    gx_MDataQueue = osMessageQueueNew(MAX_CHANNELS, sizeof(ch_metadata_t), &mdata_queue);

    // 创建元数据路由任务
    const osThreadAttr_t MDataRouterTask_attr = {
        .name       = "MDataRouterTask",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };
    MDataRouterHandle = osThreadNew(MDataRouterTask, NULL, &MDataRouterTask_attr);

    IapHandle      = osThreadNew(IapTask, NULL, &IapTask_attributes);
    ProtocolHandle = osThreadNew(ProtocolTask, NULL, &ProtocolTask_attributes);
}

void MDataRouterTask(void *argument)
{
    ch_metadata_t metadata = {0};

    for (;;) {
        osMessageQueueGet(gx_MDataQueue, &metadata, NULL, osWaitForever);

        for (uint8_t cnt = 0; cnt < PROTOCOL_CNT; cnt++) {
            if ((metadata.protocol >> cnt) & 0x00000001) {
                osMessageQueuePut(ParserQueue[cnt], &metadata, 0, 100);
            }
        }
    }
}

/**
 * @brief 通用发送函数
 *
 * @param mdata 接收到的元数据
 * @param data 要发送的数据
 * @param len 要发送的数据长度
 */
void CH_UniversalSend(ch_metadata_t *mdata, uint8_t *data, uint16_t len)
{
    switch (mdata->type) {
        case CH_TYPE_UART:
            HAL_UART_Transmit(mdata->handle.uart.huart, data, len, 100);
            break;

        case CH_TYPE_TCP:
            netconn_write(mdata->handle.tcp.conn, data, len, NETCONN_COPY);
            break;

        case CH_TYPE_UDP:
            struct netbuf *nb = netbuf_new();
            netbuf_ref(nb, data, len);
            netconn_sendto(mdata->handle.udp.conn, nb, &(mdata->handle.udp.src_ip), mdata->handle.udp.src_port);
            netbuf_delete(nb);
            break;

        case CH_TYPE_MQTT:
            mqtt_send_data(mdata->handle.mqtt.topic, (char *)data);
            break;

        default:
            break;
    }
}
