/**
 * @file    app_udp.c
 * @brief   UDP 广播接收通道（端口固定 10011，宏 LDI_DISCOVERY_PORT）
 *
 * 同端口承载两类协议（由 app_dispatch 按帧头分流）：
 *   - 创迪发现口：LDI 21H/12H 搜索
 *   - IAP 升级：0x5A5A5A5A 帧
 */

#include "app_udp.h"
#include "app_dispatch.h"
#include "app_ldi.h"
#include "app_board_net_cfg.h"
#include "pl_net.h"
#include "pl_net_adapt.h"

/* ---- 配置 ---- */
/* 端口固定（宏 LDI_DISCOVERY_PORT，创迪发现口 + IAP 共用），设计原则禁止修改接口 */
static uint16_t g_udp_port = LDI_DISCOVERY_PORT;

uint16_t app_udp_get_port(void)
{
    return g_udp_port;
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
static const osThreadAttr_t udp_connect_attr = {
    .name       = "udp_connect_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 文件级单实例通道：UAF 根治 ----
 * 连接任务退出后栈上通道实例悬垂是历史 UAF 根源。通道实例提为文件级
 * static，ch 指针生命周期恒定；连接任务重入时复用同一实例（init 重新
 * 拷贝模板重置状态）。已注销/未注册的旧通知由 dispatch 侧
 * app_channel_get 回验丢弃（app_dispatch.c）。 */
static udp_channel_t s_udp_ch;

/* ---- 广播发送（2026-08-21 修复 LDI 搜索广播丢包）----
 * 优先复用常驻通道 conn（udp_task 创建后已 ip_set_option(SOF_BROADCAST) 并
 * netconn_bind 驻留），不占 netconn 池——dev 共存构建 baseline 4 netconn
 * 已满（UDP 10011 + UDP 20103 + TCP Server listener + TCP Client），旧实现
 * 每次广播临时 netconn_new 必然 NULL → 静默丢包（LDI 12H 搜索应答 50-75%
 * 无响应）。常驻 conn 未就绪（断链重建窗口，deinit 已置 conn=NULL）时回退
 * 临时 conn 路径（池满时 netconn_new 仍可能 NULL → 静默，与旧行为一致）。
 * netconn API 经 tcpip 线程 mailbox 投递，任务上下文调用安全；发送与断链
 * deinit/netconn_delete 的并发窗口与 channel_send 现状一致（已知限制）。 */
static err_t _udp_broadcast_send(struct netconn *conn, const uint8_t *data, uint16_t len, uint16_t port)
{
    ip_addr_t bc_addr;
    IP4_ADDR(&bc_addr, 255, 255, 255, 255);

    struct netbuf *nb = netbuf_new();
    if (nb == NULL)
        return ERR_MEM;

    netbuf_ref(nb, data, len);
    err_t err = netconn_sendto(conn, nb, &bc_addr, port);
    netbuf_delete(nb);
    return err;
}

void app_udp_broadcast(const uint8_t *data, uint16_t len)
{
    /* 常驻 conn 就绪（通道仍注册 + conn 非空）→ 复用常驻 conn 广播 */
    if (app_channel_get(CH_ID_UDP) == &s_udp_ch.me && s_udp_ch.conn != NULL) {
        (void)_udp_broadcast_send((struct netconn *)s_udp_ch.conn, data, len, g_udp_port);
        return;
    }

    /* 回退：临时 conn 路径（保留 NULL 检查，池满时静默失败） */
    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL)
        return;

    ip_set_option(conn->pcb.udp, SOF_BROADCAST);
    (void)_udp_broadcast_send(conn, data, len, g_udp_port);
    netconn_delete(conn);
}

/* ---- UDP 通道 ops （注意：不能命名为 udp_send，与 LwIP 内部符号冲突）---- */
static int32_t udp_ch_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    udp_channel_t *udp = container_of(ch, udp_channel_t, me);
    struct netbuf *nb  = netbuf_new();
    if (!nb)
        return -1;

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
    (void)argument;
    if (udp_disconnect_sem == NULL)
        udp_disconnect_sem = osSemaphoreNew(1, 0, NULL);
    pl_net_register_link_listener(udp_link_listener);

    for (;;) {
        struct netconn *conn = netconn_new(NETCONN_UDP);
        if (conn == NULL) {
            osDelay(2000);
            continue;
        }

        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        err_t err = netconn_bind(conn, IP_ADDR_ANY, g_udp_port);

        if (err == ERR_OK) {
            /* 清空残留断开信号，避免刚起来就误退 */
            while (osSemaphoreAcquire(udp_disconnect_sem, 0) == osOK);

            osThreadId_t tid = osThreadNew(udp_connect_task, conn, &udp_connect_attr);
            if (tid != NULL) {
                /* 链路断开 / recv 失败后由 connect_task 释放信号量 */
                osSemaphoreAcquire(udp_disconnect_sem, osWaitForever);
            }
        }

        /* 每次循环新建 netconn：delete 后不可再 bind 旧句柄 */
        netconn_delete(conn);
        osDelay(2000);
    }
}

/* ---- 构造 / 析构 ---- */
static void udp_channel_init(udp_channel_t *self, struct netconn *conn, channel_t *tmpl)
{
    self->me          = *tmpl;
    self->me.state    = CH_STATE_UP;
    self->conn        = conn;
    self->listen_port = g_udp_port;
    app_channel_register(CH_ID_UDP, &self->me);
}

static void udp_channel_deinit(udp_channel_t *self)
{
    self->me.ops   = nullptr;
    self->me.state = CH_STATE_DOWN;
    self->conn     = NULL; /* 广播复用路径判空依据（2026-08-21） */
    app_channel_register(CH_ID_UDP, nullptr);
}

void udp_connect_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    udp_channel_init(&s_udp_ch, conn, &g_udp_channel_tmpl);

    channel_t *ch = &s_udp_ch.me;
    struct netbuf *buf;
    err_t err;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        void *data;
        uint16_t len;
        do {
            netbuf_data(buf, &data, &len);
            if (len > 0) {
                const ip_addr_t *addr = netbuf_fromaddr(buf);
                s_udp_ch.src_ip[0]    = ip4_addr1((const ip4_addr_t *)addr);
                s_udp_ch.src_ip[1]    = ip4_addr2((const ip4_addr_t *)addr);
                s_udp_ch.src_ip[2]    = ip4_addr3((const ip4_addr_t *)addr);
                s_udp_ch.src_ip[3]    = ip4_addr4((const ip4_addr_t *)addr);
                s_udp_ch.src_port     = netbuf_fromport(buf);
                app_channel_dispatch(ch, (uint8_t *)data, len);
            }
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    udp_channel_deinit(&s_udp_ch);
    osSemaphoreRelease(udp_disconnect_sem);
    osThreadExit();
}

/* ================================================================
 *  CQ 业务口 UDP 通道（CH_ID_UDP_CQ）
 *
 *  完全镜像 10011 通道的 netconn 收发 / 断链重建模式。
 *  端口语义（见 _udp_cq_read_port，方案 B 2026-08-21）：
 *    - PROTO_CHONGQING（CQ 量产构建）：读 Sector1 net_cfg.udp_port（CQ UDP
 *      业务口专有字段，CQ setip 写入；空扇区由 app_net_boot 写 20103 出厂
 *      默认），有效（1~65535）即用，无效/0 回退 UDP_CQ_DEFAULT_PORT(20103)；
 *    - 其余（dev 共存构建，LDI 编入）：固定 20103——net_cfg.udp_port 在 dev
 *      构建无写入方（CQ setip 双构建语义见 doc/03 PartB B.7.1），固定
 *      20103 保证与 CQ 量产口径一致。
 *  业务口承载 CQ JSON 业务 + 12B 二进制重启/搜索请求。
 * ================================================================ */

#define UDP_CQ_DEFAULT_PORT (20103U)

static uint16_t g_udp_cq_port = UDP_CQ_DEFAULT_PORT;

uint16_t app_udp_cq_get_port(void)
{
    return g_udp_cq_port;
}

/* ---- 信号量资源 ---- */
static osSemaphoreId_t udp_cq_disconnect_sem;

/* ---- 链路监听器 ---- */
static void udp_cq_link_listener(bool link_up)
{
    if (!link_up && udp_cq_disconnect_sem)
        osSemaphoreRelease(udp_cq_disconnect_sem);
}

/* ---- 前向声明 ---- */
void udp_cq_connect_task(void *argument);

/* ---- 连接任务属性 ---- */
static const osThreadAttr_t udp_cq_connect_attr = {
    .name       = "udp_cq_connect_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/* ---- 文件级单实例通道（UAF 根治，与 10011 通道同模式）---- */
static udp_channel_t s_udp_cq_ch;

/* 常驻 conn 复用广播（2026-08-21，语义同 app_udp_broadcast 见上）：
 * udp_cq_task 创建常驻 conn 时已设 SOF_BROADCAST；常驻未就绪回退临时 conn。 */
void app_udp_cq_broadcast(const uint8_t *data, uint16_t len)
{
    if (app_channel_get(CH_ID_UDP_CQ) == &s_udp_cq_ch.me && s_udp_cq_ch.conn != NULL) {
        (void)_udp_broadcast_send((struct netconn *)s_udp_cq_ch.conn, data, len, g_udp_cq_port);
        return;
    }

    struct netconn *conn = netconn_new(NETCONN_UDP);
    if (conn == NULL)
        return;

    ip_set_option(conn->pcb.udp, SOF_BROADCAST);
    (void)_udp_broadcast_send(conn, data, len, g_udp_cq_port);
    netconn_delete(conn);
}

static int32_t udp_cq_ch_send(channel_t *ch, const uint8_t *data, uint16_t len)
{
    udp_channel_t *udp = container_of(ch, udp_channel_t, me);
    struct netbuf *nb  = netbuf_new();
    if (!nb)
        return -1;

    netbuf_ref(nb, data, len);
    struct netconn *conn = (struct netconn *)udp->conn;
    ip_addr_t addr;
    IP4_ADDR(&addr, udp->src_ip[0], udp->src_ip[1], udp->src_ip[2], udp->src_ip[3]);
    err_t err = netconn_sendto(conn, nb, &addr, udp->src_port);
    netbuf_delete(nb);
    return (err == ERR_OK) ? (int32_t)len : -1;
}

static const ch_ops_t udp_cq_ch_ops = {
    .send = udp_cq_ch_send,
};

/* 通道元数据模板（ch_id = CH_ID_UDP_CQ） */
static channel_t s_udp_cq_channel_tmpl = {
    .ch_id = CH_ID_UDP_CQ,
    .ops   = &udp_cq_ch_ops,
};

const osThreadAttr_t udp_cq_task_attr = {
    .name       = "udp_cq_task",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/** @brief 确定 CQ 业务口端口（udp_cq_task 每轮重建前调用，setip 重启后生效）。
 *
 *  PROTO_CHONGQING（CQ 量产构建）：读 Sector1 net_cfg.udp_port——CQ UDP 业务口
 *  专有字段（方案 B 2026-08-21，CQ setip 写入；空扇区由 app_net_boot 写 20103
 *  出厂默认），有效（1~65535）即用，无效/0 回退 UDP_CQ_DEFAULT_PORT(20103)。
 *
 *  其余构建（dev 共存，LDI 编入）：固定 UDP_CQ_DEFAULT_PORT，不读 net_cfg.udp_port
 *  ——dev 构建 udp_port 无写入方，读残留记录会与 CQ 量产口径不一致。
 */
static void _udp_cq_read_port(void)
{
#ifdef PROTO_CHONGQING
    app_board_net_cfg_t cfg;
    if (app_board_net_cfg_get(&cfg) == 0 && cfg.udp_port > 0U && cfg.udp_port <= 65535U)
        g_udp_cq_port = (uint16_t)cfg.udp_port;
    else
        g_udp_cq_port = UDP_CQ_DEFAULT_PORT;
#else
    g_udp_cq_port = UDP_CQ_DEFAULT_PORT;
#endif
}

void udp_cq_task(void *argument)
{
    (void)argument;
    if (udp_cq_disconnect_sem == NULL)
        udp_cq_disconnect_sem = osSemaphoreNew(1, 0, NULL);
    pl_net_register_link_listener(udp_cq_link_listener);

    for (;;) {
        _udp_cq_read_port(); /* 每轮重建前重读端口（setip 重启后生效） */

        struct netconn *conn = netconn_new(NETCONN_UDP);
        if (conn == NULL) {
            osDelay(2000);
            continue;
        }

        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        err_t err = netconn_bind(conn, IP_ADDR_ANY, g_udp_cq_port);

        if (err == ERR_OK) {
            /* 清空残留断开信号，避免刚起来就误退 */
            while (osSemaphoreAcquire(udp_cq_disconnect_sem, 0) == osOK);

            osThreadId_t tid = osThreadNew(udp_cq_connect_task, conn, &udp_cq_connect_attr);
            if (tid != NULL) {
                /* 链路断开 / recv 失败后由 connect_task 释放信号量 */
                osSemaphoreAcquire(udp_cq_disconnect_sem, osWaitForever);
            }
        }

        /* 每次循环新建 netconn：delete 后不可再 bind 旧句柄 */
        netconn_delete(conn);
        osDelay(2000);
    }
}

/* ---- 构造 / 析构 ---- */
static void udp_cq_channel_init(udp_channel_t *self, struct netconn *conn, channel_t *tmpl)
{
    self->me          = *tmpl;
    self->me.state    = CH_STATE_UP;
    self->conn        = conn;
    self->listen_port = g_udp_cq_port;
    app_channel_register(CH_ID_UDP_CQ, &self->me);
}

static void udp_cq_channel_deinit(udp_channel_t *self)
{
    self->me.ops   = nullptr;
    self->me.state = CH_STATE_DOWN;
    self->conn     = NULL; /* 广播复用路径判空依据（2026-08-21） */
    app_channel_register(CH_ID_UDP_CQ, nullptr);
}

void udp_cq_connect_task(void *argument)
{
    struct netconn *conn = (struct netconn *)argument;

    udp_cq_channel_init(&s_udp_cq_ch, conn, &s_udp_cq_channel_tmpl);

    channel_t *ch = &s_udp_cq_ch.me;
    struct netbuf *buf;
    err_t err;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        void *data;
        uint16_t len;
        do {
            netbuf_data(buf, &data, &len);
            if (len > 0) {
                const ip_addr_t *addr = netbuf_fromaddr(buf);
                s_udp_cq_ch.src_ip[0]    = ip4_addr1((const ip4_addr_t *)addr);
                s_udp_cq_ch.src_ip[1]    = ip4_addr2((const ip4_addr_t *)addr);
                s_udp_cq_ch.src_ip[2]    = ip4_addr3((const ip4_addr_t *)addr);
                s_udp_cq_ch.src_ip[3]    = ip4_addr4((const ip4_addr_t *)addr);
                s_udp_cq_ch.src_port     = netbuf_fromport(buf);
                app_channel_dispatch(ch, (uint8_t *)data, len);
            }
        } while (netbuf_next(buf) >= 0);
        netbuf_delete(buf);
    }

    udp_cq_channel_deinit(&s_udp_cq_ch);
    osSemaphoreRelease(udp_cq_disconnect_sem);
    osThreadExit();
}
