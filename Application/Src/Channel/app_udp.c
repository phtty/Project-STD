/**
 * @file    app_udp.c
 * @brief       UDP 广播接收通道（监听端口 10011）
 *
 * 通道控制块是静态对象，连接信息（conn）挂在它上面：链路断开只清 conn、
 * 置 state，控制块本身始终有效，因此协议侧保存的 ccb_t* 永不悬空。
 *
 * 容器：typedef struct { ccb_t base; void *conn; ... } udp_ccb_t;
 */

#include "app_udp.h"
#include "app_dispatch.h"
#include "pl_net.h"
#include "pl_net_adapt.h"
#include "pl_task.h"
#include "net_diag.h"

/* ---- 配置 ---- */
static uint16_t g_udp_port = 10011; /**< IAP 升级通道 */

void app_udp_set_port(uint16_t port)
{
    g_udp_port = port;
}

uint16_t app_udp_get_port(void)
{
    return g_udp_port;
}

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

/* 这里曾注册一个链路监听器，在物理链路断开时释放 udp_disconnect_sem。
 * 它与 Platform 侧的通知机制一起删掉了 —— 理由见 Platform/Src/pl_net.c 顶部那段：
 * 通知本身从来没生效过，而且**不需要**：socket 一直绑着、netconn_recv 一直阻塞，
 * 实测拔插网线自愈。这个信号量留给 udp_task 自己重连用（bind 失败等场景）。 */

/* ---- 前向声明 ---- */
void udp_connect_task(void *argument);

/* ---- 连接任务属性 ---- */
const osThreadAttr_t udp_connect_attr = {
    .name       = "udp_connect_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- UDP 通道 ops （注意：不能命名为 udp_send，与 LwIP 内部符号冲突）---- */
static int32_t udp_ccb_send(ccb_t *ccb, const ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    udp_ccb_t *udp = container_of(ccb, udp_ccb_t, base);
    /* 未连接：conn 可能是即将释放的 netconn，交给接收路径清理，这里直接丢弃 */
    if (udp->base.state != CCB_STATE_UP || udp->conn == nullptr)
        return -1;

    struct netbuf *nb = netbuf_new();
    if (!nb)
        return -1;

    netbuf_ref(nb, data, len);
    /* 从字节数组重建 ip_addr_t，隔离 middleware 类型 */
    struct netconn *conn = (struct netconn *)udp->conn;
    ip_addr_t addr;
    /* 寻址意图由协议给出，通道负责翻译：
     *   dst == nullptr      → 回复到本帧来源（源地址在接收路径上刷新）；
     *   dst->broadcast      → 255.255.255.255（如 IAP 的 cmd01/cmd02 应答）；
     *   其余                → 退化为回复本帧来源。 */
    if (dst != nullptr && dst->broadcast)
        IP4_ADDR(&addr, 255, 255, 255, 255);
    else
        IP4_ADDR(&addr, udp->src_ip[0], udp->src_ip[1], udp->src_ip[2], udp->src_ip[3]);
    err_t err = netconn_sendto(conn, nb, &addr, udp->src_port);
    NET_DIAG("TX  -> %s len=%u err=%s", (dst != nullptr && dst->broadcast) ? "广播" : "本帧来源",
             (unsigned)len, err == ERR_OK ? "ok" : lwip_strerr(err));
    netbuf_delete(nb);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

const ccb_ops_t udp_ccb_ops = {
    .send = udp_ccb_send,
};

/* ---- 通道控制块（静态持有：协议绑定期即存在，断线也不失效） ---- */
static udp_ccb_t g_udp = {
    .base = {.name = "udp", .ops = &udp_ccb_ops},
};

ccb_t *app_udp_ccb(void)
{
    return &g_udp.base;
}

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
    (void)argument;
    if (udp_disconnect_sem == NULL)
        udp_disconnect_sem = osSemaphoreNew(1, 0, NULL);

    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        osThreadExit();
        return;
    }

    for (;;) {
        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        err_t err = netconn_bind(conn, IP_ADDR_ANY, g_udp_port);

        NET_DIAG("udp bind :%u -> %s", (unsigned)g_udp_port, err == ERR_OK ? "ok" : lwip_strerr(err));

        if (err == ERR_OK) {
            while (osSemaphoreAcquire(udp_disconnect_sem, 0) == osOK);

            osThreadId_t tid = pl_task_new(udp_connect_task, conn, &udp_connect_attr);
            if (tid != NULL)
                osSemaphoreAcquire(udp_disconnect_sem, osWaitForever);
        }

        netconn_delete(conn);
        osDelay(2000);
    }
}

/* ---- 连接生命周期：控制块始终有效，只有 conn / state 随连接生灭 ---- */
void udp_connect_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    udp_ccb_t *udp   = &g_udp;
    udp->conn        = conn;
    udp->listen_port = g_udp_port;
    udp->base.state  = CCB_STATE_UP;

    struct netbuf *buf;
    err_t err;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        void *data;
        uint16_t len;
        do {
            netbuf_data(buf, &data, &len);
            if (len > 0) {
                /* 先记下本帧来源：send(NULL dst) 的回包目标就是它 */
                const ip_addr_t *addr = netbuf_fromaddr(buf);
                udp->src_ip[0]        = ip4_addr1((const ip4_addr_t *)addr);
                udp->src_ip[1]        = ip4_addr2((const ip4_addr_t *)addr);
                udp->src_ip[2]        = ip4_addr3((const ip4_addr_t *)addr);
                udp->src_ip[3]        = ip4_addr4((const ip4_addr_t *)addr);
                udp->src_port         = netbuf_fromport(buf);
                NET_DIAG("RX  <- %u.%u.%u.%u:%u len=%u", udp->src_ip[0], udp->src_ip[1],
                         udp->src_ip[2], udp->src_ip[3], (unsigned)udp->src_port, (unsigned)len);
                app_ccb_dispatch(&udp->base, nullptr, (uint8_t *)data, len);
            }
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    /* 先置 DOWN 再清 conn：send 路径据此拒绝访问即将释放的 netconn */
    udp->base.state = CCB_STATE_DOWN;
    udp->conn       = nullptr;
    osSemaphoreRelease(udp_disconnect_sem);
    osThreadExit();
}
