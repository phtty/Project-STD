#include "tcp_client_app.h"

#include "lwip.h"
#include "lwip/tcp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

#include "protocol.h"

// 瀹氫箟澶勭悊tcp_client杩炴帴浠诲姟鍙ユ焺
osThreadId_t tcpClientTaskHandle;
const osThreadAttr_t tcpClientTask_attributes = {
    .name       = "tcpClientTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

// 瀹氫箟鍏蜂綋閫氫俊鏈嶅姟浠诲姟鍙ユ焺
const osThreadAttr_t tcpClientConn_attr = {
    .name       = "tcpClientConnTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

uint8_t dst_ip[]  = {192, 168, 114, 100};
uint16_t dst_port = 9529;
osSemaphoreId_t ClientDiscnnSemaphore;

void tcpClientConnTask(void *argument);
__STATIC_INLINE void tcp_keepaliveinit(struct netconn *conn);

/**
 * @brief 澶勭悊tcp_client閲嶈繛绠＄悊浠诲姟
 *
 * @param argument
 */
void tcpClientTask(void *argument)
{
    struct netconn *conn;
    err_t err;
    ip_addr_t server_addr;

    // 绛夊緟缃戠粶鎺ュ彛灏辩华
    while (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)))
        osDelay(100);

    if (ClientDiscnnSemaphore == NULL) { // 鍒涘缓淇″彿閲忎互妫娴嬫柇寮
        ClientDiscnnSemaphore = osSemaphoreNew(1, 0, NULL);
    }

    IP4_ADDR(&server_addr, dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);

    for (;;) {
        // 鍒涘缓涓涓鏂扮殑tcp杩炴帴鍙ユ焺
        conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) {
            osDelay(1000);
            continue;
        }

        printf("Client: Attempting to connect IP: %d.%d.%d.%d ...\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        err = netconn_connect(conn, &server_addr, dst_port);

        if (err == ERR_OK) {
            printf("Client: Connected successfully!\n");

            // 纭淇濅俊鍙烽噺鏄绌虹殑
            while (osSemaphoreAcquire(ClientDiscnnSemaphore, 0) == osOK);

            ch_meta_t meta = {
                .type            = CH_TYPE_TCP,
                .protocol        = PROTO_MASK_NONE,
                .handle.tcp.conn = conn,
            };
            osThreadId_t thread_id = osThreadNew(tcpClientConnTask, (void *)&meta, &tcpClientConn_attr);

            if (thread_id != NULL) { // 闃诲炲湪杩欓噷锛岀洿鍒伴氫俊浠诲姟閫氱煡杩炴帴鏂寮
                osSemaphoreAcquire(ClientDiscnnSemaphore, osWaitForever);
            }

        } else {
            printf("Client: Connect failed, err == %d\n", (int)err);
        }

        // 鍒犻櫎鏃у彞鏌勶紝鍑嗗囦笅涓娆￠噸杩
        netconn_close(conn);
        netconn_delete(conn);
        conn = NULL;
        osDelay(1000); // 閬垮厤杩囦簬棰戠箒鐨勯噸杩
    }
}

/**
 * @brief 澶勭悊鍏蜂綋閫氫俊鏈嶅姟浠诲姟(Client绔鏁版嵁澶勭悊)
 *
 * @param argument 宸茬粡杩炴帴鎴愬姛鐨刵etconn鎸囬拡
 */
void tcpClientConnTask(void *argument)
{
    ch_meta_t *meta      = (ch_meta_t *)argument;
    struct netconn *conn = meta->handle.tcp.conn;
    struct netbuf *buf;
    err_t err;
    void *data;
    uint16_t len;

    // 涓鸿繖涓杩炴帴娉ㄥ唽keepalive
    tcp_keepaliveinit(conn);

    // 寰鐜鎺ユ敹鏁版嵁
    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        // 涓寸晫淇濇姢鍖猴紝闃叉㈠氫釜淇￠亾鍚屾椂鍐檙ingbuff

        do {
            netbuf_data(buf, &data, &len);

            // 鏀惧叆鐜褰㈢紦鍐插尯渚涘崗璁鏍/涓氬姟閫昏緫浣跨敤
            if (len > 0) {
                rb_write(&g_proto_rb, (uint8_t *)data, len, g_proto_rb.mutex);
            }

        } while (netbuf_next(buf) >= 0);

        // 鍏冩暟鎹鍏ラ槦
        osMessageQueuePut(g_meta_queue, meta, 0, 100);


        netbuf_delete(buf); // 閲婃斁netbuf鍐呭瓨
    }

    // 鍙戦佷俊鍙烽噺閫氱煡杩炴帴宸叉柇寮锛屽彲浠ュ皾璇曢噸杩
    osSemaphoreRelease(ClientDiscnnSemaphore);
    printf("Client: Connection lost, reason: %d\n", (int)err);

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
