#include "tcp_sever.h"

#include "lwip/sockets.h"
#include "lwip/mem.h"
#include "RingBuffer.h"

osThreadId_t tcpServerTaskHandle;
const osThreadAttr_t tcpServerTask_attributes = {
    .name       = "tcpServerTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

const osThreadAttr_t tcpSeverConn_attr = {
    .name       = "tcpServerConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

__STATIC_INLINE void tcp_keepaliveinit(int client_sock);

RingBuffer xProtocal_RB = {0};
void tcpServerConnTask(void *argument)
{
    int client_sock = (int)argument; // 转换回socket句柄
    int read_len;

    // 为这个连接注册keepalive
    tcp_keepaliveinit(client_sock);

    uint8_t *recv_buffer = mem_malloc(RECV_BUFFER_SIZE); // 动态分配缓冲区，节省静态内存
    if (recv_buffer == NULL) {
        close(client_sock);
        osThreadExit();
        return;
    }

    for (;;) { // 数据处理循环
        read_len = recv(client_sock, recv_buffer, RECV_BUFFER_SIZE - 1, 0);

        if (read_len > 0) { // 放入环形缓冲区，准备做协议解析
            BSP_RB_PutByte_Bulk(&xProtocal_RB, recv_buffer, read_len);

        } else { // 连接断开或错误会退出接收数据循环
            break;
        }
    }

    // 清理工作
    mem_free(recv_buffer);
    close(client_sock);

    // 任务自行结束并销毁
    osThreadExit();
}

void tcpServerTask(void *argument)
{
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;

    for (;;) {
        if (netif_default != NULL) // 检查 LwIP 是否有默认网卡
            // 检查网卡是否“UP”（驱动已启用且物理链路连接正常）
            if (netif_is_up(netif_default) && netif_is_link_up(netif_default))
                // 检查是否获取到了有效 IP (不是 0.0.0.0)
                if (!ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
                    break;

        // 每500ms尝试检查一次连接是否建立
        osDelay(500);
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (server_sock < 0) { // 创建失败
        osThreadExit();
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_len         = sizeof(server_addr);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);      // 监听所有网卡接口 IP
    server_addr.sin_port        = htons(TCP_SERVER_PORT); // 端口号需转换为网络字节序

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_sock);
        osThreadExit();
        return;
    }

    if (listen(server_sock, 5) < 0) {
        close(server_sock);
        osThreadExit();
        return;
    }

    for (;;) {
        client_addr_len = sizeof(client_addr);

        // 阻塞在这里，直到有新client
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0)
            continue;

        // 新的client连接会创建一个独立的任务来处理数据收发
        osThreadId_t thread_id = osThreadNew(tcpServerConnTask, (void *)client_sock, &tcpSeverConn_attr);

        if (thread_id == NULL) // 任务创建失败，关闭这个连接
            close(client_sock);
    }
}

__STATIC_INLINE void tcp_keepaliveinit(int client_sock)
{
    int keepalive    = true; // keepalive使能
    int keepidle     = 10;   // 空闲后开始探测
    int keepinterval = 2;    // 每次探测间隔
    int keepcount    = 3;    // 尝试 3 次失败后断开

    // 使能keepalive
    // 参数含义：套接字选项级别（确定该选项适用范围），选项名称，选项值指针（设定的值），参数长度
    setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    // 设置keepalive探测参数
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepinterval, sizeof(keepinterval));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));
}
