/**
 * @file    dev_tcp_client.c
 * @brief       TCP 客户端通道（连接 192.168.114.100:9529）
 *
 * 通道控制块是静态对象，连接信息（conn）挂在它上面：断线只清 conn、置 state，
 * 控制块本身始终有效，因此协议侧保存的 ccb_t* 永不悬空。
 * 远端地址属于模块配置而非通道身份，故放文件级静态变量。
 *
 * 容器：typedef struct { ccb_t base; void *conn; } tcp_ccb_t;（与 server 共用）
 */

#include "app_tcp_client.h"
#include "app_dispatch.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

/* ---- 信号量 ---- */
osSemaphoreId_t client_disconnect_sem;

/* ---- 前向声明 ---- */
void tcp_client_conn_task(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

/* ---- 连接任务属性 ---- */
const osThreadAttr_t tcp_client_conn_attr = {
    .name       = "tcp_client_conn_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

osThreadId_t tcp_client_task_handle;
const osThreadAttr_t tcp_client_task_attr = {
    .name       = "tcp_client_task",
    .stack_size = 512 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 通道控制块（静态持有：协议绑定期即存在，断线也不失效） ---- */
static tcp_ccb_t g_tcp_client = {
    .base = {.name = "tcp_client", .ops = &tcp_ccb_ops},
};

ccb_t *app_tcp_client_ccb(void)
{
    return &g_tcp_client.base;
}

/* ---- 远端配置（属模块配置，不是通道身份，故不放进控制块） ---- */
static uint8_t s_host_ip[4] = {192, 168, 2, 17};
static uint16_t s_host_port = 9529;

/* ---- 配置接口 ---- */
__attribute__((used)) void app_tcp_client_set_remote(const uint8_t ip[4], uint16_t port)
{
    memcpy(s_host_ip, ip, 4);
    s_host_port = port;
    /* 配置变更即打断当前连接，让主任务用新地址重连 */
    if (client_disconnect_sem != nullptr)
        osSemaphoreRelease(client_disconnect_sem);
}

uint8_t *app_tcp_client_get_host_ip(void)
{
    return s_host_ip;
}

uint16_t app_tcp_client_get_host_port(void)
{
    return s_host_port;
}

/* ================================================================
 *  主任务：connect → 派生连接任务 → 重连循环
 * ================================================================ */

void tcp_client_task(void *argument)
{
    (void)argument;
    if (client_disconnect_sem == NULL)
        client_disconnect_sem = osSemaphoreNew(1, 0, NULL);

    for (;;) {
        struct netconn *conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) {
            osDelay(1000);
            continue;
        }

        ip_addr_t server_addr;
        IP4_ADDR(&server_addr, s_host_ip[0], s_host_ip[1], s_host_ip[2], s_host_ip[3]);

        /* 非阻塞 connect：避免 netconn_connect 在无服务器时永久阻塞 */
        netconn_set_nonblocking(conn, 1);
        err_t err = netconn_connect(conn, &server_addr, s_host_port);

        if (err == ERR_OK || err == ERR_INPROGRESS) {
            /* 轮询等待连接完成（15 秒超时） */
            uint32_t deadline = osKernelGetTickCount() + 4000;
            bool connected    = false;

            while ((int32_t)(osKernelGetTickCount() - deadline) < 0) {
                if (conn->state != NETCONN_CONNECT) {
                    connected = (conn->pcb.tcp != NULL);
                    break;
                }
                osDelay(500);
            }

            if (connected) {
                netconn_set_nonblocking(conn, 0); /* 连接已建立，恢复阻塞模式供 recv 使用 */

                while (osSemaphoreAcquire(client_disconnect_sem, 0) == osOK);

                osThreadId_t tid = pl_task_new(tcp_client_conn_task, conn, &tcp_client_conn_attr);
                if (tid != NULL)
                    osSemaphoreAcquire(client_disconnect_sem, osWaitForever);

                netconn_close(conn);
                netconn_delete(conn);
                osDelay(1000);
                continue;
            }
        }

        /* 连接失败或超时：删除 netconn 后重试 */
        netconn_delete(conn);
        osDelay(1000);
    }
}

void tcp_client_conn_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    /* 只把本连接的 conn 挂到静态控制块上，控制块本身不被连接生灭牵动 */
    tcp_ccb_t *tcp  = &g_tcp_client;
    tcp->conn       = conn;
    tcp->base.state = CCB_STATE_UP;
    tcp_keepaliveinit(conn);

    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        do {
            netbuf_data(buf, &data, &len);
            if (len > 1)
                app_ccb_dispatch(&tcp->base, nullptr, (uint8_t *)data, len);
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    /* 先置 DOWN 再清 conn：send 路径据此拒绝访问即将释放的 netconn */
    tcp->base.state = CCB_STATE_DOWN;
    tcp->conn       = nullptr;
    osSemaphoreRelease(client_disconnect_sem);
    osThreadExit();
}

__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn)
{
    if (conn == NULL || conn->pcb.tcp == NULL) return;
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);
    conn->pcb.tcp->keep_idle  = 10000;
    conn->pcb.tcp->keep_intvl = 2000;
    conn->pcb.tcp->keep_cnt   = 3;
}
