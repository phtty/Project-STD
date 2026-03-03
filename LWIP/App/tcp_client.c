#include "tcp_client.h"

#include "lwip.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"
#include "RingBuffer.h"

// 定义处理tcp_client连接任务句柄
osThreadId_t tcpClientTaskHandle;
const osThreadAttr_t tcpClientTask_attributes = {
    .name       = "tcpClientTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

// 定义具体通信服务任务句柄
const osThreadAttr_t tcpClientConn_attr = {
    .name       = "tcpClientConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

uint8_t dst_ip[]  = {192, 168, 114, 100};
uint16_t dst_port = 9529;
osSemaphoreId_t ClientDiscnnSemaphore;

void tcpClientConnTask(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

/**
 * @brief 处理tcp_client重连管理任务
 *
 * @param argument
 */
void tcpClientTask(void *argument)
{
    struct netconn *conn;
    err_t err;
    ip_addr_t server_addr;

    // 等待网络接口就绪事件标志
    osEventFlagsWait(netEventFlagsHandle, FLAG_NET_READY, osFlagsWaitAny | osFlagsNoClear, osWaitForever);

    IP4_ADDR(&server_addr, dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);

    for (;;) {
        // 创建一个新的 netconn
        conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) {
            osDelay(1000);
            continue;
        }

        printf("Client: Attempting to connect...\n");
        err = netconn_connect(conn, &server_addr, dst_port);

        if (err == ERR_OK) {
            printf("Client: Connected successfully!\n");

            // 这个信号量由tcpClientConnTask在检测到连接断开/recv返回非ERR_OK时释放
            ClientDiscnnSemaphore = osSemaphoreNew(1, 0, NULL);

            osThreadId_t thread_id = osThreadNew(tcpClientConnTask, (void *)conn, &tcpClientConn_attr);

            if (thread_id != NULL) { // 使用信号量等待连接断开
                osSemaphoreAcquire(ClientDiscnnSemaphore, osWaitForever);
            }

        } else {
            printf("Client: Connect failed, err = %d\n", (int)err);
        }

        // 删除旧句柄，准备下一次重连
        netconn_close(conn);
        netconn_delete(conn);
        conn = NULL;
        osDelay(1000); // 避免过于频繁的重连
    }
}

/**
 * @brief 处理具体通信服务任务(Client端数据处理)
 *
 * @param argument 已经连接成功的netconn指针
 */
void tcpClientConnTask(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;
    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    // 为这个连接注册keepalive
    tcp_keepaliveinit(conn);

    // 循环接收数据
    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        // 遍历处理netbuf中的片段
        do {
            netbuf_data(buf, &data, &len);

            // 放入环形缓冲区供协议栈/业务逻辑使用
            if (len > 0) {
                BSP_RB_PutByte_Bulk(&xProtocal_RB, (uint8_t *)data, len);
            }

        } while (netbuf_next(buf) >= 0);

        netbuf_delete(buf); // 释放 netbuf 内存
    }

    // 清理并关闭连接
    netconn_close(conn);
    netconn_delete(conn);

    // 发送信号量通知连接已断开，可以尝试重连
    osSemaphoreRelease(ClientDiscnnSemaphore);
    printf("Client: Connection lost/closed...\n");

    // 任务自行结束
    osThreadExit();
}

__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn)
{
    if (conn == NULL || conn->pcb.tcp == NULL)
        return;

    // 开启 Keepalive 标志
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);

    // 设置具体参数 (单位通常为毫秒)
    conn->pcb.tcp->keep_idle  = 10000; // 10秒空闲后开始探测
    conn->pcb.tcp->keep_intvl = 2000;  // 2秒探测一次
    conn->pcb.tcp->keep_cnt   = 3;     // 3次失败后断开
}
