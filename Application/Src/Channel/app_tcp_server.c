/**
 * @file    app_tcp_server.c
 * @brief   TCP 服务器通道 — 串行单客户端服务循环
 *
 * 单连接任务循环：bind → listen → accept → 服务该客户端（recv 循环）
 * → 客户端断开 → 回 accept 下一客户端。
 *
 * UAF 根治：通道实例为文件级 static 单实例，生命周期内注册一次、
 * 注销仅在服务循环退出时；ch 指针恒定，dispatch 侧 app_channel_get
 * 回验兜底。原「每客户端一 conn 任务」的并发派生模式已移除。
 */

#include "app_tcp_server.h"

#include "FreeRTOS.h"
#include "app_dispatch.h"
#include "pl_net_adapt.h"

#define TCP_SERVER_PORT 9528

/* ---- TCP 通道虚表：send = netconn_write ---- */
static int32_t tcp_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    tcp_server_channel_t *tcp = container_of(ch, tcp_server_channel_t, me);
    err_t err                 = netconn_write((struct netconn *)tcp->conn, data, len, NETCONN_COPY);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const ch_ops_t tcp_ch_ops = {.send = tcp_send};

/* ---- 通道元数据模板 ---- */
channel_t g_tcp_server_channel_tmpl = {
    .ch_id = CH_ID_TCP_SERVER,
    .ops   = &tcp_ch_ops,
};

/* ---- 文件级 static 单实例通道（UAF 根治）：
 * 每客户端不再新建实例/任务，ch 指针生命周期恒定；
 * 已注销的旧通知由 dispatch 侧 app_channel_get 回验丢弃。 */
static tcp_server_channel_t s_tcp_ch;

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

static void tcp_channel_deinit(tcp_server_channel_t *self)
{
    self->me.ops   = nullptr; /* 防止 send 路径访问即将释放的 netconn */
    self->me.state = CH_STATE_DOWN;
    app_channel_register(CH_ID_TCP_SERVER, nullptr);
}

/* ================================================================
 *  服务任务: bind → listen → accept → 服务客户端 → 断开 → 回 accept
 * ================================================================ */

void tcp_server_task(void *argument)
{
    (void)argument;

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

        /* 串行单客户端服务循环：同一 listener 反复 accept */
        for (;;) {
            struct netconn *newconn = NULL;
            err                     = netconn_accept(conn, &newconn);
            if (err != ERR_OK || newconn == NULL) {
                /* 监听异常 → 重建 listener */
                osDelay(500);
                break;
            }

            /* 服务该客户端直至断开 */
            tcp_channel_init(&s_tcp_ch, newconn, &g_tcp_server_channel_tmpl);
            g_tcp_server_connected = 1;

            channel_t *ch = &s_tcp_ch.me;
            struct netbuf *buf;
            void *data;
            uint16_t len;

            while (netconn_recv(newconn, &buf) == ERR_OK) {
                do {
                    netbuf_data(buf, &data, &len);
                    if (len > 1)
                        app_channel_dispatch(ch, (uint8_t *)data, len);
                } while (netbuf_next(buf) >= 0);
                netbuf_delete(buf);
            }

            g_tcp_server_connected = 0;
            tcp_channel_deinit(&s_tcp_ch);
            netconn_close(newconn);
            netconn_delete(newconn);
            /* 回 accept 下一客户端 */
        }

        netconn_delete(conn);
    }
}