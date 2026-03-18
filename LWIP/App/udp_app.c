#include "udp_app.h"

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

#include "msg.h"
#include "iap.h"

// 定义处理udp连接的任务句柄
osThreadId_t udpManageTaskHandle;
const osThreadAttr_t udpManageTask_attributes = {
    .name       = "udpManageTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

// 定义具体通信服务任务句柄
const osThreadAttr_t udpConnect_attr = {
    .name       = "udpConnectTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

osSemaphoreId_t udpDiscnnSemaphore;

void udpConnectTask(void *argument);

/**
 * @brief 处理udp_client管理任务
 *
 * @param argument
 */
void udpManageTask(void *argument)
{
    struct netconn *conn;
    err_t err;

    while (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)))
        osDelay(100); // 等待网络就绪

    if (udpDiscnnSemaphore == NULL) { // 创建信号量以检测断开
        udpDiscnnSemaphore = osSemaphoreNew(1, 0, NULL);
    }

    for (;;) {
        conn = netconn_new(NETCONN_UDP); // 创建一个UDP连接句柄
        if (conn == NULL) {
            osDelay(1000);
            continue;
        }

        // 开启广播
        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        // 监听所有网卡上的数据，包括广播
        err = netconn_bind(conn, IP_ADDR_ANY, UDP_PORT);

        if (err == ERR_OK) {
            printf("UDP Receiver: Listening on port %d (Broadcast enabled)\n", (int)UDP_PORT);

            // 确保信号量已被释放，同时申请一个空闲的信道
            while (osSemaphoreAcquire(udpDiscnnSemaphore, 0) == osOK);
            int16_t channel_index                      = Channel_Alloc(CH_TYPE_UDP);
            gx_Channels[channel_index].handle.udp.conn = conn;

            // 创建数据接收任务
            udp_conn_arg_t arg = {
                .conn          = conn,
                .channel_index = channel_index,
            };
            osThreadId_t thread_id = osThreadNew(udpConnectTask, (void *)&arg, &udpConnect_attr);

            if (thread_id != NULL) { // 阻塞等待断开连接信号量
                osSemaphoreAcquire(udpDiscnnSemaphore, osWaitForever);
                Channel_Free(channel_index); // 断开连接同时释放信道
            }
        } else {
            printf("UDP Receiver: Bind failed, err == %d\n", (int)err);
        }

        netconn_delete(conn);
        osDelay(2000);
    }
}

/**
 * @brief 处理具体通信服务任务(UDP数据接收)
 *
 * @param argument 已经绑定的netconn指针
 */
void udpConnectTask(void *argument)
{
    struct netconn *conn = ((udp_conn_arg_t *)argument)->conn;
    struct netbuf *buf;
    err_t err;

    IapHandle = osThreadNew(IapTask, NULL, &IapTask_attributes);

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        MsgQueueItem_t q_item;
        q_item.channel_index = ((udp_conn_arg_t *)argument)->channel_index;
        q_item.session_id    = gx_Channels[((udp_conn_arg_t *)argument)->channel_index].session_id;
        q_item.udp_src_ip    = *netbuf_fromaddr(buf); // 存入队列，不存全局
        q_item.udp_src_port  = netbuf_fromport(buf);

        // 临界保护区，防止多个信道同时写ringbuff
        osMutexAcquire(gx_RingBufMutex, osWaitForever);

        void *data;
        uint16_t len;
        do { // 接收数据并放入环形缓冲区
            netbuf_data(buf, &data, &len);

            if (len > 0) {
                // BSP_RB_PutByte_Bulk(&xProtocol_RB, (uint8_t *)data, len);
                BSP_RB_PutByte_Bulk(&xIAP_RB, (uint8_t *)data, len);
            }
        } while (netbuf_next(buf) >= 0);

        osMessageQueuePut(gx_PacketMsg, &q_item, 0, 100);

        osMutexRelease(gx_RingBufMutex);

        netbuf_delete(buf);
    }

    osSemaphoreRelease(udpDiscnnSemaphore);
    osThreadExit();
}
