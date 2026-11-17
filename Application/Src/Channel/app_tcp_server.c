/**
 * @file    app_tcp_server.c
 * @brief   TCP 服务器通道 — 可实例化的 manage 任务 + conn 任务
 *
 * 参照 tcp_client 模式:
 *   app_tcp_server_task       bind→listen→accept→派生conn→等待断开→循环
 *   app_tcp_server_conn_task  netconn_recv→dispatch，断开时释放信号量
 *
 * 实例化：静态池两个槽位——默认实例（9528，LDI 用，API 不变）+ 一个扩展实例
 * （AH/TCP 参数口 6001）。每个实例有自己的 ccb / 端口 / 断开信号量；
 * 同一实例同一时刻只服务一个连接（与改造前的单例模型一致）。
 *
 * 容器：typedef struct { app_ccb_t base; void *conn; } app_tcp_ccb_t;
 */

#include <stdbool.h>
#include <stdio.h>

#include "app_tcp_server.h"

#include "app_dispatch.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

/* ---- 实例（对头文件不透明） ---- */
struct app_tcp_server {
    app_tcp_ccb_t    ccb;               /**< 通道控制块（base 在 ccb 首位） */
    uint16_t         port;              /**< 监听端口 */
    uint32_t         idle_timeout_ms;   /**< 连接空闲超时；0 = 不超时 */
    osSemaphoreId_t  disconnect_sem;    /**< 连接断开通知（实例内单连接） */
    osThreadId_t     task_handle;       /**< 管理任务句柄 */
    bool             started;           /**< 防止重复启动 */
};

static app_tcp_server_t s_default_srv; /**< 默认实例（9528） */
static app_tcp_server_t s_extra_srv;   /**< 扩展实例（AH 参数口） */
static bool             s_default_created;
static bool             s_extra_used;
static uint16_t         s_default_port = 9528U;

/* ---- 连接任务属性（所有实例共用） ---- */
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

osThreadId_t g_tcp_server_task_handle;
const osThreadAttr_t g_tcp_server_task_attr = {
    .name       = "app_tcp_server_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ================================================================
 *  实例管理
 * ================================================================ */

/** @brief 初始化一个空实例（不建任务、不建信号量） */
static void _server_init(app_tcp_server_t *srv, const char *name, uint16_t port,
                         uint32_t idle_timeout_ms)
{
    srv->ccb.base.name         = name;
    srv->ccb.base.ops          = &g_tcp_ccb_ops;
    srv->ccb.base.state        = APP_CCB_STATE_DOWN;
    srv->ccb.conn              = nullptr;
    srv->port                  = port;
    srv->idle_timeout_ms       = idle_timeout_ms;
    srv->disconnect_sem        = nullptr;
    srv->task_handle           = nullptr;
    srv->started               = false;
}

/** @brief 取默认实例；首次调用时按当前 `s_default_port` 惰性创建 */
static app_tcp_server_t *_default_server(void)
{
    if (!s_default_created) {
        _server_init(&s_default_srv, "tcp_server", s_default_port, 0U);
        s_default_created = true;
    }
    return &s_default_srv;
}

app_tcp_server_t *app_tcp_server_open(const char *name, uint16_t port, uint32_t idle_timeout_ms)
{
    if (name == nullptr || port == 0U || s_extra_used)
        return nullptr;

    _server_init(&s_extra_srv, name, port, idle_timeout_ms);
    s_extra_used = true;
    return &s_extra_srv;
}

app_ccb_t *app_tcp_server_ccb_of(const app_tcp_server_t *srv)
{
    if (srv == nullptr)
        return nullptr;
    return (app_ccb_t *)&srv->ccb.base;
}

app_ccb_t *app_tcp_server_ccb(void)
{
    return app_tcp_server_ccb_of(_default_server());
}

void app_tcp_server_set_port(uint16_t port)
{
    s_default_port = port;
    if (s_default_created)
        s_default_srv.port = port;
}

uint16_t app_tcp_server_get_port(void)
{
    return s_default_port;
}

osThreadId_t app_tcp_server_start_instance(app_tcp_server_t *srv)
{
    if (srv == nullptr)
        return nullptr;
    if (srv->started)
        return srv->task_handle;

    osThreadId_t tid = osThreadNew(app_tcp_server_task, srv, &g_tcp_server_task_attr);
    if (tid != nullptr) {
        srv->task_handle = tid;
        srv->started     = true;
        if (srv == &s_default_srv)
            g_tcp_server_task_handle = tid;
    }
    return tid;
}

osThreadId_t app_tcp_server_start(void)
{
    app_tcp_server_t *srv = _default_server();

    /* 本板没有任何协议绑定默认实例（如 5006048 走 AH/UDP，不跑 LDI）：
       不要空开一个监听任务 + netconn + 1KB 栈。 */
    if (srv->ccb.base.proto_cnt == 0U) {
        printf("[tcp_svr] 默认实例未绑定协议，不启动 :%u\n", (unsigned)srv->port);
        return nullptr;
    }
    return app_tcp_server_start_instance(srv);
}

/* ================================================================
 *  manage 任务: bind → listen → accept → 派生 conn → 等待断开 → 循环
 * ================================================================ */

void app_tcp_server_task(void *argument)
{
    app_tcp_server_t *srv = (app_tcp_server_t *)argument;
    if (srv == nullptr) {
        osThreadExit();
        return;
    }

    if (srv->disconnect_sem == nullptr)
        srv->disconnect_sem = osSemaphoreNew(1, 0, nullptr);

    for (;;) {
        struct netconn *conn = netconn_new(NETCONN_TCP);
        if (conn == nullptr) {
            osDelay(500);
            continue;
        }

        err_t err = netconn_bind(conn, IP_ADDR_ANY, srv->port);
        if (err != ERR_OK) {
            printf("[tcp_svr] %s bind :%u 失败（err=%d）\n", srv->ccb.base.name,
                   (unsigned)srv->port, (int)err);
            netconn_delete(conn);
            osDelay(500);
            continue;
        }

        err = netconn_listen(conn);
        if (err != ERR_OK) {
            printf("[tcp_svr] %s listen :%u 失败（err=%d）\n", srv->ccb.base.name,
                   (unsigned)srv->port, (int)err);
            netconn_delete(conn);
            osDelay(500);
            continue;
        }

        /* accept 一个连接 */
        struct netconn *newconn = nullptr;
        err                     = netconn_accept(conn, &newconn);
        netconn_delete(conn);

        if (err != ERR_OK || newconn == nullptr) {
            osDelay(500);
            continue;
        }

        /* 同一实例同一时刻只有一个连接：把 conn 挂到实例上，再派生 conn 任务 */
        while (osSemaphoreAcquire(srv->disconnect_sem, 0) == osOK);
        srv->ccb.conn = newconn;

        osThreadId_t tid = pl_task_new(app_tcp_server_conn_task, srv, &s_tcp_server_conn_attr);
        if (tid != nullptr) {
            osSemaphoreAcquire(srv->disconnect_sem, osWaitForever);
        } else {
            printf("[tcp_svr] %s conn 任务创建失败\n", srv->ccb.base.name);
            srv->ccb.conn = nullptr;
            netconn_close(newconn);
            netconn_delete(newconn);
            osDelay(500);
        }
    }
}

/* ================================================================
 *  连接的 keepalive
 *
 *  **必须开**：对端消失（拔网线/关机/进程被 kill）不会发 FIN，`netconn_recv`
 *  会一直阻塞；而监听用的 PCB 在 accept 之后就已经关掉了 —— 于是既没有监听、
 *  也等不到断开，现场表现就是"UDP 还能搜到设备、TCP 怎么都连不上"，只能断电。
 *
 *  10s 空闲 + 2s×3 次探测：活着的空闲连接由协议栈自动回 ACK，不受影响
 *  （上位机放着不动也不会被踢）；死连接约 16s 内被回收，manage 任务随即重新
 *  listen。与 `app_tcp_client.c` 的同名逻辑保持一致。
 * ================================================================ */
static void _tcp_server_keepalive(struct netconn *conn)
{
    if (conn == nullptr || conn->pcb.tcp == nullptr)
        return;
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);
    conn->pcb.tcp->keep_idle  = 10000;
    conn->pcb.tcp->keep_intvl = 2000;
    conn->pcb.tcp->keep_cnt   = 3;
}

/* ================================================================
 *  conn 任务: netconn_recv → dispatch，断开时释放信号量
 * ================================================================ */

void app_tcp_server_conn_task(void *argument)
{
    app_tcp_server_t *srv = (app_tcp_server_t *)argument;
    if (srv == nullptr || srv->ccb.conn == nullptr) {
        /* 早退也必须放信号量：否则 manage 任务会永远等在这个连接上，监听口死掉 */
        if (srv != nullptr && srv->disconnect_sem != nullptr)
            osSemaphoreRelease(srv->disconnect_sem);
        osThreadExit();
        return;
    }

    struct netconn *conn = (struct netconn *)srv->ccb.conn;
    srv->ccb.base.state  = APP_CCB_STATE_UP;
    _tcp_server_keepalive(conn);

    /* 空闲超时：只有显式配置了才设（默认实例保持无限等待 + keepalive） */
    if (srv->idle_timeout_ms > 0U)
        netconn_set_recvtimeout(conn, srv->idle_timeout_ms);

    struct netbuf *buf;
    void          *data;
    uint16_t       len;

    while (netconn_recv(conn, &buf) == ERR_OK) {
        do {
            netbuf_data(buf, &data, &len);
            if (len > 1)
                app_ccb_dispatch(&srv->ccb.base, nullptr, (uint8_t *)data, len);
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    /* 先置 DOWN 再清 conn：send 路径据此拒绝访问即将释放的 netconn */
    srv->ccb.base.state = APP_CCB_STATE_DOWN;
    srv->ccb.conn       = nullptr;
    netconn_close(conn);
    netconn_delete(conn);
    osSemaphoreRelease(srv->disconnect_sem);
    osThreadExit();
}
