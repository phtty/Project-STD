/**
 * @file    app_tcp_server.c
 * @brief   TCP 服务器通道 — manage 任务 + conn 任务
 *
 * 参照 tcp_client 模式:
 *   tcp_server_task       bind→listen→accept→派生conn→等待断开→循环
 *   tcp_server_conn_task  netconn_recv→dispatch，断开时释放信号量
 */

#include "app_tcp_server.h"

#include "app_dispatch.h"
#include "pl_net_adapt.h"

#define TCP_SERVER_PORT 9528

/* ---- 信号量 ---- */
static osSemaphoreId_t s_disconnect_sem;

/* ---- 连接任务属性 ---- */
const osThreadAttr_t tcp_server_conn_attr = {
    .name       = "tcp_svr_conn",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- TCP 通道虚表：send = netconn_write ---- */
static int32_t tcp_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    tcp_server_channel_t *tcp = container_of(ch, tcp_server_channel_t, me);
    err_t err                 = netconn_write((struct netconn *)tcp->conn, data, len, NETCONN_COPY);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const ch_ops_t tcp_ch_ops = {.send = tcp_send};

/* ---- 通道元数据模板（每连接 copy） ---- */
channel_t g_tcp_server_channel_tmpl = {
    .ch_id = CH_ID_TCP_SERVER,
    .ops   = &tcp_ch_ops,
};

/* ---- 配置接口 ---- */
static uint16_t g_port = TCP_SERVER_PORT;

void app_tcp_server_set_port(uint16_t port)
{ g_port = port; }
uint16_t app_tcp_server_get_port(void)
{ return g_port; }

osThreadId_t tcp_server_task_handle;
const osThreadAttr_t tcp_server_task_attr = {
    .name       = "tcp_server_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 调试变量 ---- */
volatile int g_tcp_server_connected;

/* ---- 通道生命周期 ---- */
static void tcp_channel_init(tcp_server_channel_t *self, void *conn, channel_t *tmpl)
{
    self->me       = *tmpl;
    self->me.state = CH_STATE_UP;
    self->conn     = conn;
    app_channel_register(CH_ID_TCP_SERVER, &self->me);
}

/* ================================================================
 *  manage 任务: bind → listen → accept → 派生 conn → 等待断开 → 循环
 * ================================================================ */

void tcp_server_task(void *argument)
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

        err_t err = netconn_bind(conn, IP_ADDR_ANY, g_port);
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
        osThreadId_t tid = osThreadNew(tcp_server_conn_task, newconn, &tcp_server_conn_attr);

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
 * ================================================================ */

void tcp_server_conn_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    tcp_server_channel_t tcp;
    tcp_channel_init(&tcp, conn, &g_tcp_server_channel_tmpl);

    channel_t *ch          = &tcp.me;
    g_tcp_server_connected = 1;

    struct netbuf *buf;
    void *data;
    uint16_t len;

    while (netconn_recv(conn, &buf) == ERR_OK) {
        do {
            netbuf_data(buf, &data, &len);
            if (len > 1)
                app_channel_dispatch(ch, (uint8_t *)data, len);
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    g_tcp_server_connected = 0;
    tcp.me.ops             = nullptr;
    tcp.me.state           = CH_STATE_DOWN;
    app_channel_register(CH_ID_TCP_SERVER, nullptr);
    netconn_close(conn);
    netconn_delete(conn);
    osSemaphoreRelease(s_disconnect_sem);
    osThreadExit();
}
