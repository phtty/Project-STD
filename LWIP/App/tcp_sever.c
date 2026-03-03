#include "tcp_sever.h"

#include "lwip.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

// 定义处理tcp_server连接任务句柄
osThreadId_t tcpServerTaskHandle;
const osThreadAttr_t tcpServerTask_attributes = {
    .name       = "tcpServerTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

// 定义具体通信服务任务句柄
const osThreadAttr_t tcpSeverConn_attr = {
    .name       = "tcpServerConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

void tcpServerConnTask(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

/**
 * @brief 处理tcp_server连接任务
 *
 * @param argument
 */
void tcpServerTask(void *argument)
{
    struct netconn *conn, *newconn;
    err_t err;

    // 阻塞等待事件标志
    while (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)))
        osDelay(100);

    // 创建一个TCP netconn句柄
    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL) {
        printf("Server: Create netconn handler failed!\n");
        osThreadExit();
        return;
    }

    // 绑定端口
    err = netconn_bind(conn, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK) {
        printf("Server: Bind port failed\n");
        netconn_delete(conn);
        osThreadExit();
        return;
    }

    // 进入监听模式
    netconn_listen(conn);

    for (;;) {
        // 阻塞等待新连接
        err = netconn_accept(conn, &newconn);

        if (err == ERR_OK) {
            // 创建新任务处理该连接
            osThreadId_t thread_id = osThreadNew(tcpServerConnTask, (void *)newconn, &tcpSeverConn_attr);
            if (thread_id == NULL) {
                netconn_close(newconn);
                netconn_delete(newconn);
            }

        } else {
            // 如果 accept 出错，5s后重试
            netconn_delete(newconn);
            osDelay(5000);
        }
    }
}

/**
 * @brief 处理具体通信服务任务
 *
 * @param argument
 */
void tcpServerConnTask(void *argument)
{
    struct netconn *newconn = (struct netconn *)argument;
    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    // 为这个连接注册keepalive
    tcp_keepaliveinit(newconn);

    // 循环接收数据
    while ((err = netconn_recv(newconn, &buf)) == ERR_OK) { // netconn_recv会阻塞，直到收到数据或连接断开
        // Netconn 的数据存储在 netbuf 中，可能包含多个片段
        do {
            netbuf_data(buf, &data, &len); // 获取当前片段的数据指针和长度

            // 放入环形缓冲区
            if (len > 0)
                BSP_RB_PutByte_Bulk(&xProtocal_RB, (uint8_t *)data, len);

        } while (netbuf_next(buf) >= 0); // 移动到下一个片段

        netbuf_delete(buf); // 处理完后必须释放 netbuf
    }

    // 清理并关闭连接
    netconn_close(newconn);
    netconn_delete(newconn);

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
