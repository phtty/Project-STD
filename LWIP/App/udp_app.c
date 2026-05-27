#include "udp_app.h"

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/api.h"
#include "lwip/mem.h"

#include "protocol.h"
#include "iap.h"

// 瀹氫箟澶勭悊udp杩炴帴鐨勪换鍔″彞鏌
osThreadId_t udpManageTaskHandle;
const osThreadAttr_t udpManageTask_attributes = {
    .name       = "udpManageTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

// 瀹氫箟鍏蜂綋閫氫俊鏈嶅姟浠诲姟鍙ユ焺
const osThreadAttr_t udpConnect_attr = {
    .name       = "udpConnectTask",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

osSemaphoreId_t udpDiscnnSemaphore;

void udpConnectTask(void *argument);

/**
 * @brief 澶勭悊udp_client绠＄悊浠诲姟
 *
 * @param argument
 */
void udpManageTask(void *argument)
{
    struct netconn *conn;
    err_t err;

    while (!(netif_is_up(netif_default) && netif_is_link_up(netif_default)))
        osDelay(100); // 绛夊緟缃戠粶灏辩华

    if (udpDiscnnSemaphore == NULL) { // 鍒涘缓淇″彿閲忎互妫娴嬫柇寮
        udpDiscnnSemaphore = osSemaphoreNew(1, 0, NULL);
    }

    for (;;) {
        conn = netconn_new(NETCONN_UDP); // 鍒涘缓涓涓猆DP杩炴帴鍙ユ焺
        if (conn == NULL) {
            osDelay(1000);
            continue;
        }

        // 寮鍚骞挎挱
        ip_set_option(conn->pcb.udp, SOF_BROADCAST);
        // 鐩戝惉鎵鏈夌綉鍗′笂鐨勬暟鎹锛屽寘鎷骞挎挱
        err = netconn_bind(conn, IP_ADDR_ANY, UDP_PORT);

        if (err == ERR_OK) {
            printf("UDP Receiver: Listening on port %d (Broadcast enabled)\n", (int)UDP_PORT);

            // 纭淇濅俊鍙烽噺宸茶閲婃斁
            while (osSemaphoreAcquire(udpDiscnnSemaphore, 0) == osOK);
            ch_meta_t meta = {
                .type            = CH_TYPE_UDP,
                .protocol        = PROTO_MASK_IAP,
                .handle.udp.conn = conn,
            };

            // 鍒涘缓鏁版嵁鎺ユ敹浠诲姟
            osThreadId_t thread_id = osThreadNew(udpConnectTask, (void *)&meta, &udpConnect_attr);

            if (thread_id != NULL) { // 闃诲炵瓑寰呮柇寮杩炴帴淇″彿閲
                osSemaphoreAcquire(udpDiscnnSemaphore, osWaitForever);
            }
        } else {
            printf("UDP Receiver: Bind failed, err == %d\n", (int)err);
        }

        netconn_delete(conn);
        osDelay(2000);
    }
}

/**
 * @brief 澶勭悊鍏蜂綋閫氫俊鏈嶅姟浠诲姟(UDP鏁版嵁鎺ユ敹)
 *
 * @param argument 宸茬粡缁戝畾鐨刵etconn鎸囬拡
 */
void udpConnectTask(void *argument)
{
    ch_meta_t *meta      = (ch_meta_t *)argument;
    struct netconn *conn = meta->handle.udp.conn;
    struct netbuf *buf;
    err_t err;

    while ((err = netconn_recv(conn, &buf)) == ERR_OK) {
        // 涓寸晫淇濇姢鍖猴紝闃叉㈠氫釜淇￠亾鍚屾椂鍐檙ingbuff

        void *data;
        uint16_t len;
        do { // 鎺ユ敹鏁版嵁骞舵斁鍏ョ幆褰㈢紦鍐插尯
            netbuf_data(buf, &data, &len);

            if (len > 0) {
                // rb_write(&g_proto_rb, (uint8_t *)data, len, g_proto_rb.mutex);
                rb_write(&g_iap_rb, (uint8_t *)data, len, g_iap_rb.mutex);
            }
        } while (netbuf_next(buf) >= 0);

        // 鍏冩暟鎹鍏ラ槦
        meta->handle.udp.src_ip   = *netbuf_fromaddr(buf);
        meta->handle.udp.src_port = netbuf_fromport(buf);
        osMessageQueuePut(g_meta_queue, meta, 0, 100);


        netbuf_delete(buf);
    }

    osSemaphoreRelease(udpDiscnnSemaphore);
    osThreadExit();
}
