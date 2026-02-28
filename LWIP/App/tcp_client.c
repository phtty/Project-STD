#include "tcp_client.h"

// 定义处理tcp_client连接任务句柄
osThreadId_t tcpClientTaskHandle;
const osThreadAttr_t tcpClientTask_attributes = {
    .name       = "tcpClientTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

// 定义具体通信服务任务句柄
const osThreadAttr_t tcpClientConn_attr = {
    .name       = "tcpClientConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);
