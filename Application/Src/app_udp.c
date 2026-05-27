/**
 * @file    dev_udp.c
 * @brief       UDP 广播接收通道（监听端口 10011）
 */

#include "app_udp.h"
#include "app_dispatch.h"
#include "pl_net.h"
#include "pl_net_adapt.h"

/* ---- 配置 ---- */
static uint16_t g_udp_port = 10011; /**< IAP 升级通道 */

void app_udp_set_port(uint16_t port) { g_udp_port = port; }
uint16_t app_udp_get_port(void) { return g_udp_port; }

void app_udp_broadcast(const uint8_t *data, uint16_t len)
{
    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) return;

    ip_set_option(conn->pcb.udp, SOF_BROADCAST);
    ip_addr_t bc_addr;
    IP4_ADDR(&bc_addr, 255, 255, 255, 255);

    struct netbuf *nb = netbuf_new();
    if (nb) {
        netbuf_ref(nb, data, len);
        netconn_sendto(conn, nb, &bc_addr, g_udp_port);
        netbuf_delete(nb);
    }
    netconn_delete(conn);
}

/* ---- 信号量资源 ---- */
static osSemaphoreId_t udp_disconnect_sem;

/* ---- 链路监听器：物理链路断开时释放信号量，udp_task 收到后设 ch.state=DOWN ---- */
static void udp_link_listener(bool link_up)
{
    if (!link_up && udp_disconnect_sem)
        osSemaphoreRelease(udp_disconnect_sem);
}

/* ---- 前向声明 ---- */
void udp_connect_task(void *argument);

/* ---- 连接任务属性 ---- */
const osThreadAttr_t udp_connect_attr = {
    .name       = "udp_connect_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- UDP 通道 ops （注意：不能命名为 udp_send，与 LwIP 内部符号冲突）---- */
static int32_t udp_ch_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    udp_channel_t *udp = container_of(ch, udp_channel_t, ch);
    struct netbuf *nb  = netbuf_new();
    if (!nb) return -1;
    netbuf_ref(nb, data, len);
    /* 从字节数组重建 ip_addr_t，隔离 middleware 类型 */
    struct netconn *conn = (struct netconn *)udp->conn;
    ip_addr_t addr;
    IP4_ADDR(&addr, udp->src_ip[0], udp->src_ip[1], udp->src_ip[2], udp->src_ip[3]);
    err_t err = netconn_sendto(conn, nb, &addr, udp->src_port);
    netbuf_delete(nb);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const ch_ops_t udp_ch_ops = {
    .send = udp_ch_send,
};

/* ---- 通道元数据模板（ch_id 由 Application 层设置，每 bind 后 copy 并填入 handle） ---- */
channel_t g_udp_channel_tmpl = {
    .ch_id = CH_ID_UDP,
    .ops   = &udp_ch_ops,
};

osThreadId_t udp_task_handle;
const osThreadAttr_t udp_task_attr = {
    .name       = "udp_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ================================================================
 *  实现
 * ================================================================ */
void udp_task(void *argument)
{
    if (udp_disconnect_sem == NULL)
        udp_disconnect_sem = osSemaphoreNew(1, 0, NULL);
    pl_net_register_link_listener(udp_link_listener);

    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        osThreadExit();
        return;
    }

    for (;;) {
        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        err_t err = netconn_bind(conn, IP_ADDR_ANY, g_udp_port);

        if (err == ERR_OK) {
            while (osSemaphoreAcquire(udp_disconnect_sem, 0) == osOK);

            osThreadId_t tid = osThreadNew(udp_connect_task, conn, &udp_connect_attr);
            if (tid != NULL)
                osSemaphoreAcquire(udp_disconnect_sem, osWaitForever);
        }

        netconn_delete(conn);
        osDelay(2000);
    }
}

/* ---- 构造 / 析构 ---- */
static void udp_channel_init(udp_channel_t *self, struct netconn *conn, channel_t *tmpl)
{
    self->ch         = *tmpl;
    self->ch.state   = CH_STATE_UP;
    self->conn       = conn;
    self->listen_port = g_udp_port;
    app_channel_register(CH_ID_UDP, &self->ch);
}

static void udp_channel_deinit(udp_channel_t *self)
{
    self->ch.ops   = nullptr;
    self->ch.state = CH_STATE_DOWN;
    app_channel_register(CH_ID_UDP, nullptr);
}

void udp_connect_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    udp_channel_t udp;
    udp_channel_init(&udp, conn, &g_udp_channel_tmpl);

    channel_t *ch = &udp.ch;
    struct netbuf *buf;
    err_t err;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        void *data;
        uint16_t len;
        do {
            netbuf_data(buf, &data, &len);
            if (len > 0) {
                const ip_addr_t *addr = netbuf_fromaddr(buf);
                udp.src_ip[0] = ip4_addr1((const ip4_addr_t *)addr);
                udp.src_ip[1] = ip4_addr2((const ip4_addr_t *)addr);
                udp.src_ip[2] = ip4_addr3((const ip4_addr_t *)addr);
                udp.src_ip[3] = ip4_addr4((const ip4_addr_t *)addr);
                udp.src_port   = netbuf_fromport(buf);
                app_channel_dispatch(ch, (uint8_t *)data, len);
            }
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    udp_channel_deinit(&udp);
    osSemaphoreRelease(udp_disconnect_sem);
    osThreadExit();
}
