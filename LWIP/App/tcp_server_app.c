#include "tcp_server_app.h"

#include "lwip.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

#include "protocol.h"

// 瀹氫箟澶勭悊tcp_server杩炴帴浠诲姟鍙ユ焺
osThreadId_t tcpServerTaskHandle;
const osThreadAttr_t tcpServerTask_attributes = {
    .name       = "tcpServerTask",
    .stack_size = 256 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

// 瀹氫箟鍏蜂綋閫氫俊鏈嶅姟浠诲姟鍙ユ焺
const osThreadAttr_t tcpSeverConn_attr = {
    .name       = "tcpServerConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

void tcpServerConnTask(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

/**
 * @brief 澶勭悊tcp_server杩炴帴浠诲姟
 *
 * @param argument
 */
void tcpServerTask(void *argument)
{
    struct netconn *conn, *newconn;
    err_t err;

    // 绛夊緟缃戠粶鎺ュ彛灏辩华
    while (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)))
        osDelay(100);

    // 鍒涘缓涓涓猅CP netconn鍙ユ焺
    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL) {
        printf("Server: Create netconn handler failed!\n");
        osThreadExit();
        return;
    }

    // 缁戝畾绔鍙
    err = netconn_bind(conn, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK) {
        printf("Server: Bind port failed\n");
        netconn_delete(conn);
        osThreadExit();
        return;
    }

    // 杩涘叆鐩戝惉妯″紡
    netconn_listen(conn);

    for (;;) {
        // 闃诲炵瓑寰呮柊杩炴帴
        err = netconn_accept(conn, &newconn);

        if (err == ERR_OK) {
            // 鍒涘缓鏂颁换鍔″勭悊璇ヨ繛鎺
            ch_meta_t meta = {
                .type            = CH_TYPE_TCP,
                .protocol        = PROTO_MASK_NONE,
                .handle.tcp.conn = newconn,
            };
            osThreadId_t thread_id = osThreadNew(tcpServerConnTask, (void *)&meta, &tcpSeverConn_attr);
            if (thread_id == NULL) {
                netconn_close(newconn);
                netconn_delete(newconn);
            }

        } else {
            // 濡傛灉 accept 鍑洪敊锛5s鍚庨噸璇
            netconn_delete(newconn);
            osDelay(5000);
        }
    }
}

/**
 * @brief 澶勭悊鍏蜂綋閫氫俊鏈嶅姟浠诲姟
 *
 * @param argument
 */
void tcpServerConnTask(void *argument)
{
    ch_meta_t *meta         = (ch_meta_t *)argument;
    struct netconn *newconn = meta->handle.tcp.conn;
    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    // 涓鸿繖涓杩炴帴娉ㄥ唽keepalive
    tcp_keepaliveinit(newconn);

    // 寰鐜鎺ユ敹鏁版嵁
    while ((err = netconn_recv(newconn, &buf)) == ERR_OK) { // netconn_recv浼氶樆濉烇紝鐩村埌鏀跺埌鏁版嵁鎴栬繛鎺ユ柇寮
        // 涓寸晫淇濇姢鍖猴紝闃叉㈠氫釜淇￠亾鍚屾椂鍐檙ingbuff

        do {
            netbuf_data(buf, &data, &len); // 鑾峰彇褰撳墠鐗囨电殑鏁版嵁鎸囬拡鍜岄暱搴

            // 鏀惧叆鐜褰㈢紦鍐插尯
            if (len > 0)
                rb_write(&g_proto_rb, (uint8_t *)data, len, g_proto_rb.mutex);

        } while (netbuf_next(buf) >= 0); // 绉诲姩鍒颁笅涓涓鐗囨

        // 鍏冩暟鎹鍏ラ槦
        osMessageQueuePut(g_meta_queue, meta, 0, 100);


        netbuf_delete(buf); // 閲婃斁netbuf
    }

    // 娓呯悊骞跺叧闂杩炴帴
    netconn_close(newconn);
    netconn_delete(newconn);

    // 浠诲姟鑷琛岀粨鏉
    osThreadExit();
}

__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn)
{
    if (conn == NULL || conn->pcb.tcp == NULL)
        return;

    // 寮鍚 Keepalive 鏍囧織
    ip_set_option(conn->pcb.tcp, SOF_KEEPALIVE);

    // 璁剧疆鍏蜂綋鍙傛暟 (鍗曚綅閫氬父涓烘绉)
    conn->pcb.tcp->keep_idle  = 10000; // 10绉掔┖闂插悗寮濮嬫帰娴
    conn->pcb.tcp->keep_intvl = 2000;  // 2绉掓帰娴嬩竴娆
    conn->pcb.tcp->keep_cnt   = 3;     // 3娆″け璐ュ悗鏂寮
}
