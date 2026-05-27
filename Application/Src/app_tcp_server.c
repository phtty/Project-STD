/**
 * @file    dev_tcp_server.c
 * @brief   TCP 服务器通道 — 多端口监听 + accept + 每连接独立收发
 *
 * 两层任务架构：
 *   tcp_server_task()           → 遍历 g_listeners[]，每端口 spawn 一个 listen worker
 *   tcp_server_listen_task(ctx) → bind(ctx->port) → listen → accept 循环
 *       └─ tcp_server_conn_task(newconn) → netconn_recv → app_channel_dispatch
 *
 * 发送通过 tcp_ch_ops.send = netconn_write（NETCONN_COPY 模式）。
 */

#include "app_tcp_server.h"

#include "app_dispatch.h"
#include "pl_net_adapt.h"

/* ---- 前向声明 ---- */
void tcp_server_conn_task(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

static const osThreadAttr_t tcp_listen_attr = {
    .name       = "tcp_svr_listen",
    .stack_size = 512 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 连接任务属性 ---- */
const osThreadAttr_t tcp_server_conn_attr = {
    .name       = "tcp_svr_conn",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- TCP 通道虚表：send = netconn_write ---- */
static int32_t tcp_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    tcp_server_channel_t *tcp = container_of(ch, tcp_server_channel_t, ch);
    struct netconn *nc = (struct netconn *)tcp->conn;
    err_t err          = netconn_write(nc, data, len, NETCONN_COPY);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const ch_ops_t tcp_ch_ops = {.send = tcp_send};

/* ---- 通道元数据模板 ---- */
channel_t g_tcp_server_channel_tmpl = {
    .ch_id = CH_ID_TCP_SERVER,
    .ops   = &tcp_ch_ops,
};

/* ---- 监听器数组（每端口一个 ctx，加端口只加数组元素） ---- */
static tcp_server_listener_ctx_t g_listeners[] = {
    {.port = 9528, .max_conns = 2},
};

/* ---- 配置接口（操作第一个监听器） ---- */
void app_tcp_server_set_port(uint16_t port)
{
    g_listeners[0].port    = port;
    g_listeners[0].restart = true; /* 监听任务的 accept 超时后检测到此标志，自行重 bind */
}

uint16_t app_tcp_server_get_port(void)
{
    return g_listeners[0].port;
}

osThreadId_t tcp_server_task_handle;
const osThreadAttr_t tcp_server_task_attr = {
    .name       = "tcp_server_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ================================================================
 *  主任务：遍历监听器数组，为每端口创建 listen worker
 * ================================================================ */

void tcp_server_task(void *argument)
{
    (void)argument;
    for (size_t i = 0; i < sizeof(g_listeners) / sizeof(g_listeners[0]); i++)
        osThreadNew(tcp_server_listen_task, &g_listeners[i], &tcp_listen_attr);

    osThreadExit();
}

/* ================================================================
 *  监听任务：bind → listen → accept 循环（每端口一个）
 * ================================================================ */

void tcp_server_listen_task(void *ctx)
{
    tcp_server_listener_ctx_t *l = (tcp_server_listener_ctx_t *)ctx;

    for (;;) {
        /* ---- 创建 + bind + listen ---- */
        struct netconn *conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) {
            osThreadExit();
            return;
        }
        l->listen_conn = conn;

        ip_addr_t ip_addr;
        IP4_ADDR(&ip_addr, 0, 0, 0, 0);
        err_t err = netconn_bind(conn, &ip_addr, l->port);
        if (err != ERR_OK) {
            netconn_delete(conn);
            l->listen_conn = NULL;
            osThreadExit();
            return;
        }

        netconn_listen(conn);
        netconn_set_recvtimeout(conn, 1000); /* accept 超时 1s，用于检测 restart 标志 */

        /* ---- accept 循环 ---- */
        for (;;) {
            struct netconn *newconn = NULL;
            err                     = netconn_accept(conn, &newconn);

            if (err == ERR_OK) {
                if (l->conn_count >= l->max_conns) {
                    netconn_close(newconn);
                    netconn_delete(newconn);
                } else {
                    l->conn_count++;
                    struct {
                        struct netconn *c;
                        tcp_server_listener_ctx_t *l;
                    } arg            = {newconn, l};
                    osThreadId_t tid = osThreadNew(tcp_server_conn_task, &arg, &tcp_server_conn_attr);
                    if (tid == NULL) {
                        l->conn_count--;
                        netconn_close(newconn);
                        netconn_delete(newconn);
                    }
                }

            } else if (err == ERR_TIMEOUT) {
                /* 超时非错误：检查外部 set_port 是否要求重启 */
                if (l->restart)
                    break;
                continue;

            } else {
                if (newconn) netconn_delete(newconn);
                break; /* 真正的错误 → 退出 accept 循环 */
            }
        }

        /* ---- 清理旧 conn ---- */
        netconn_delete(conn);
        l->listen_conn = NULL;

        /* 重启标志为真 → 回外层循环重新 bind；为假 → 退出任务 */
        if (l->restart) {
            l->restart = false;
        } else {
            break;
        }
    }

    osThreadExit();
}

/* ================================================================
 *  连接处理任务：keepalive → netconn_recv → dispatch，断开时 conn_count--
 * ================================================================ */

void tcp_server_channel_init(tcp_server_channel_t *self, void *conn, channel_t *tmpl, tcp_server_listener_ctx_t *listener)
{
    self->ch       = *tmpl;
    self->ch.state = CH_STATE_UP;
    self->conn     = conn;
    self->listener = listener;
    tcp_keepaliveinit(conn);
    app_channel_register(CH_ID_TCP_SERVER, &self->ch);
}

void tcp_server_channel_deinit(tcp_server_channel_t *self)
{
    self->ch.ops   = nullptr;
    self->ch.state = CH_STATE_DOWN;
    app_channel_register(CH_ID_TCP_SERVER, nullptr);
    netconn_close((struct netconn *)self->conn);
    netconn_delete((struct netconn *)self->conn);
}

void tcp_server_conn_task(void *argument)
{
    struct netconn *newconn             = ((struct { struct netconn *c; tcp_server_listener_ctx_t *l; } *)argument)->c;
    tcp_server_listener_ctx_t *listener = ((struct { struct netconn *c; tcp_server_listener_ctx_t *l; } *)argument)->l;

    tcp_server_channel_t tcp;
    tcp_server_channel_init(&tcp, newconn, &g_tcp_server_channel_tmpl, listener);

    channel_t *ch = &tcp.ch;
    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    while ((err = netconn_recv(newconn, &buf)) == ERR_OK) {
        do {
            netbuf_data(buf, &data, &len);
            if (len > 1)
                app_channel_dispatch(ch, (uint8_t *)data, len);
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    tcp_server_channel_deinit(&tcp);
    tcp.listener->conn_count--;
    osThreadExit();
}

/* ---- TCP Keepalive 配置 ---- */
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn)
{
    if (conn == NULL || conn->pcb.tcp == NULL) return;
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);
    conn->pcb.tcp->keep_idle  = 10000;
    conn->pcb.tcp->keep_intvl = 2000;
    conn->pcb.tcp->keep_cnt   = 3;
}
