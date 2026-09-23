/**
 * @file    app_tcp_server.c
 * @brief   TCP 服务器通道 — manage 任务 + conn 任务
 *
 * 参照 tcp_client 模式:
 *   app_tcp_server_task       bind→listen→accept→派生conn→等待断开→循环
 *   app_tcp_server_conn_task  netconn_recv→dispatch，断开时释放信号量
 *
 * 通道控制块是静态对象，连接信息（conn）挂在它上面：断线只清 conn、置 state，
 * 控制块本身始终有效，因此协议侧保存的 app_ccb_t* 永不悬空。
 *
 * 容器：typedef struct { app_ccb_t base; void *conn; } app_tcp_ccb_t;
 */

#include "app_tcp_server.h"

#include "app_dispatch.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

#define TCP_SERVER_PORT 9528

/* ---- 信号量 ---- */
static osSemaphoreId_t s_disconnect_sem;

/* ---- 连接任务属性 ---- */
static const osThreadAttr_t s_tcp_server_conn_attr = {
    .name       = "tcp_svr_conn",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- TCP 通道虚表：send = netconn_write ---- */
static int32_t _tcp_send(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)dst; /* 点对点连接：目的地恒为本连接的对端，寻址请求一律忽略 */
    app_tcp_ccb_t *tcp = container_of(ccb, app_tcp_ccb_t, base);
    /* 未连接：对端已断开，conn 可能即将释放，丢弃 */
    if (tcp->base.state != APP_CCB_STATE_UP || tcp->conn == nullptr)
        return -1;
    err_t err = netconn_write((struct netconn *)tcp->conn, data, len, NETCONN_COPY);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const app_ccb_ops_t g_tcp_ccb_ops = {.send = _tcp_send};

/* ---- 通道控制块（静态持有：协议绑定期即存在，断线也不失效） ---- */
static app_tcp_ccb_t s_tcp_server_ccb = {
    .base = {.name = "tcp_server", .ops = &g_tcp_ccb_ops},
};

app_ccb_t *app_tcp_server_ccb(void)
{
    return &s_tcp_server_ccb.base;
}

/* ---- 配置接口 ---- */
static uint16_t s_port = TCP_SERVER_PORT;

void app_tcp_server_set_port(uint16_t port)
{ s_port = port; }
uint16_t app_tcp_server_get_port(void)
{ return s_port; }

osThreadId_t g_tcp_server_task_handle;
const osThreadAttr_t g_tcp_server_task_attr = {
    .name       = "app_tcp_server_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 调试变量 ---- */
static volatile int s_tcp_server_connected;

/* ================================================================
 *  manage 任务: bind → listen → accept → 派生 conn → 等待断开 → 循环
 * ================================================================ */

void app_tcp_server_task(void *argument)
{
    (void)argument;
    if (s_disconnect_sem == NULL)
        s_disconnect_sem = osSemaphoreNew(1, 0, NULL);

    for (;;) {
        struct netconn *conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) {
            osDelay(500);
            continue;
        }

        err_t err = netconn_bind(conn, IP_ADDR_ANY, s_port);
        if (err != ERR_OK) {
            netconn_delete(conn);
            osDelay(500);
            continue;
        }

        netconn_listen(conn);

        /* accept 一个连接 */
        struct netconn *newconn = NULL;
        err                     = netconn_accept(conn, &newconn);
        netconn_delete(conn);

        if (err != ERR_OK || newconn == NULL) {
            osDelay(500);
            continue;
        }

        /* 派生 conn 任务 */
        while (osSemaphoreAcquire(s_disconnect_sem, 0) == osOK);
        osThreadId_t tid = pl_task_new(app_tcp_server_conn_task, newconn, &s_tcp_server_conn_attr);

        if (tid != NULL) {
            osSemaphoreAcquire(s_disconnect_sem, osWaitForever);

        } else {
            netconn_close(newconn);
            netconn_delete(newconn);
            osDelay(500);
        }
    }
}

/* ================================================================
 *  conn 任务: netconn_recv → dispatch，断开时释放信号量
 *
 *  manage 任务在 accept 下一个连接前会等待断开信号量，因此同一时刻
 *  只有一个连接任务在跑，静态控制块不会出现两个连接争用。
 * ================================================================ */

void app_tcp_server_conn_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    /* 只把本连接的 conn 挂到静态控制块上，控制块本身不被连接生灭牵动 */
    app_tcp_ccb_t *tcp         = &s_tcp_server_ccb;
    tcp->conn              = conn;
    tcp->base.state        = APP_CCB_STATE_UP;
    s_tcp_server_connected = 1;

    struct netbuf *buf;
    void *data;
    uint16_t len;

    while (netconn_recv(conn, &buf) == ERR_OK) {
        do {
            netbuf_data(buf, &data, &len);
            if (len > 1)
                app_ccb_dispatch(&tcp->base, nullptr, (uint8_t *)data, len);
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    /* 先置 DOWN 再清 conn：send 路径据此拒绝访问即将释放的 netconn */
    s_tcp_server_connected = 0;
    tcp->base.state        = APP_CCB_STATE_DOWN;
    tcp->conn              = nullptr;
    netconn_close(conn);
    netconn_delete(conn);
    osSemaphoreRelease(s_disconnect_sem);
    osThreadExit();
}
