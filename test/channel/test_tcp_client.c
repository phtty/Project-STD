/**
 * @file    test_tcp_client.c
 * @brief   TCP 客户端通道：远端配置 / 连接超时 / 建连后派生 conn 任务 / 断线重连
 *
 * `app_tcp_client.c` 的现场症状都很"玄"：远端地址配错只表现为"永远连不上"；
 * 非阻塞 connect 的 4 秒超时算错会反复建连；conn 任务早退不放信号量则主任务
 * 永远等不到下一次重连。这些都不会在协议套件里暴露。
 *
 * 手法与 `test_tcp_server.c` 相同：**真的 LwIP 头文件** + 脚本化 netconn，
 * 主任务/conn 任务都跑在 pthread 上，无限循环由脚本里的 `pthread_exit()` 收尾。
 * 唯一额外的是时钟：`osDelay` / `osKernelGetTickCount` 用假 tick（不真睡 4 秒）。
 */

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_dispatch.h"
#include "app_tcp_client.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

/* ================================================================
 *  断言（变体 B，见 docs/测试约定.md 附录 B）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  看门狗
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似主任务/conn 任务死等）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* ================================================================
 *  RTOS 原语：只实现本模块用到的（含假 tick，超时用例不真睡 4 秒）
 * ================================================================ */

typedef struct {
    sem_t sem;
} _sem_t;

static uint32_t s_tick;
static int      s_delay_cnt;
static int      s_delay_500_cnt;
static uint32_t s_delay_ms;

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                              const osSemaphoreAttr_t *attr)
{
    (void)max_count;
    (void)attr;
    _sem_t *s = (_sem_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    if (sem_init(&s->sem, 0, initial_count) != 0) {
        free(s);
        return NULL;
    }
    return (osSemaphoreId_t)s;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    _sem_t *s = (_sem_t *)semaphore_id;
    if (s == NULL) return osErrorParameter;

    if (timeout == osWaitForever) {
        return (sem_wait(&s->sem) == 0) ? osOK : osError;
    }
    if (timeout == 0U) {
        return (sem_trywait(&s->sem) == 0) ? osOK : osErrorTimeout;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout / 1000U);
    ts.tv_nsec += (long)(timeout % 1000U) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return (sem_timedwait(&s->sem, &ts) == 0) ? osOK : osErrorTimeout;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    _sem_t *s = (_sem_t *)semaphore_id;
    if (s == NULL) return osErrorParameter;
    return (sem_post(&s->sem) == 0) ? osOK : osError;
}

osStatus_t osDelay(uint32_t ticks)
{
    s_delay_cnt++;
    s_delay_ms = ticks;
    if (ticks == 500U) s_delay_500_cnt++;
    s_tick += ticks;
    return osOK;
}

uint32_t osKernelGetTickCount(void) { return s_tick; }

void osThreadExit(void) { pthread_exit(NULL); }

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    (void)func;
    (void)argument;
    (void)attr;
    return NULL;
}

/* ================================================================
 *  生产模块本体
 *
 *  FreeRTOS 的 `__STATIC_INLINE` 在真机由 FreeRTOS.h 提供；host 的替身头不带它，
 *  这里补一个等价定义（只影响本套件的编译，不改生产代码）。
 * ================================================================ */

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif

#include "../../Application/Src/Channel/app_tcp_client.c"

/* ================================================================
 *  netconn 脚本替身
 * ================================================================ */

static struct netconn s_conn;
static struct tcp_pcb s_pcb;
static struct netbuf  s_rx_buf;

static int s_new_cnt;
static int s_connect_cnt;
static int s_delete_cnt;
static int s_close_cnt;
static int s_recv_cnt;
static int s_write_cnt;

static err_t    s_connect_ret = ERR_OK;
static bool     s_stay_connecting;
static ip_addr_t s_connect_addr;
static uint16_t s_connect_port;
static uint8_t  s_connect_nb;

static int s_disp_cnt;
static uint16_t s_disp_len[8];

static pthread_t s_conn_thread;
static bool      s_conn_thread_valid;
static uint8_t   s_recv_nb;

static void _reset_fakes(void)
{
    memset(&s_conn, 0, sizeof(s_conn));
    memset(&s_pcb, 0, sizeof(s_pcb));
    s_conn.pcb.tcp = &s_pcb;

    s_new_cnt = s_connect_cnt = s_delete_cnt = s_close_cnt = 0;
    s_recv_cnt = s_write_cnt = 0;
    s_connect_ret     = ERR_OK;
    s_stay_connecting = false;
    memset(&s_connect_addr, 0, sizeof(s_connect_addr));
    s_connect_port      = 0;
    s_connect_nb        = 0;
    s_disp_cnt          = 0;
    memset(s_disp_len, 0, sizeof(s_disp_len));
    s_conn_thread_valid = false;
    s_recv_nb           = 0;
    s_tick              = 0;
    s_delay_cnt         = 0;
    s_delay_500_cnt     = 0;
    s_delay_ms          = 0;
}

struct netconn *netconn_new_with_proto_and_callback(enum netconn_type type, u8_t proto,
                                                    netconn_callback callback)
{
    (void)proto;
    (void)callback;
    s_new_cnt++;
    if (s_new_cnt == 1) {
        s_conn.type  = type;
        s_conn.state = NETCONN_CONNECT; /* 真 netconn_new 的初态 */
        return &s_conn;
    }
    /* 脚本结束：主任务是无限循环，从这里退出线程 */
    pthread_exit(NULL);
    return NULL;
}

err_t netconn_connect(struct netconn *conn, const ip_addr_t *addr, u16_t port)
{
    s_connect_cnt++;
    s_connect_addr = *addr;
    s_connect_port = port;
    s_connect_nb   = (uint8_t)((conn->flags & NETCONN_FLAG_NON_BLOCKING) != 0);

    if (s_connect_ret == ERR_OK && !s_stay_connecting) {
        conn->state = NETCONN_NONE; /* 连接已建立 */
    }
    return s_connect_ret;
}

err_t netconn_delete(struct netconn *conn)
{
    (void)conn;
    s_delete_cnt++;
    return ERR_OK;
}

err_t netconn_close(struct netconn *conn)
{
    (void)conn;
    s_close_cnt++;
    return ERR_OK;
}

err_t netconn_recv(struct netconn *conn, struct netbuf **new_buf)
{
    (void)conn;
    s_recv_cnt++;
    if (s_recv_cnt == 1) {
        s_recv_nb = (uint8_t)((conn->flags & NETCONN_FLAG_NON_BLOCKING) != 0);
        *new_buf  = &s_rx_buf; /* 内容由 netbuf_* 桩给，结构体本身不参与 */
        return ERR_OK;
    }
    return ERR_CLSD;
}

err_t netconn_write_partly(struct netconn *conn, const void *data, size_t size, u8_t apiflags,
                           size_t *bytes_written)
{
    (void)conn;
    (void)data;
    (void)apiflags;
    s_write_cnt++;
    if (bytes_written != NULL) *bytes_written = size;
    return ERR_OK;
}

err_t netbuf_data(struct netbuf *buf, void **data, u16_t *len)
{
    (void)buf;
    static const uint8_t seg[2][16] = {{0}};
    *data = (void *)seg[0];
    *len  = (s_recv_cnt == 1) ? 8U : 0U;
    return ERR_OK;
}

s8_t netbuf_next(struct netbuf *buf)
{
    (void)buf;
    return -1; /* 本套件只测一包一段；多段在 test_tcp_server 里钉 */
}

void netbuf_delete(struct netbuf *buf) { (void)buf; }

/* 派发只记录 */
void app_ccb_dispatch(const app_ccb_t *ccb, const app_ccb_src_t *src, const uint8_t *data,
                      uint16_t len)
{
    (void)ccb;
    (void)src;
    (void)data;
    if (s_disp_cnt < (int)(sizeof(s_disp_len) / sizeof(s_disp_len[0])))
        s_disp_len[s_disp_cnt++] = len;
}

/* conn 任务：真的 pthread，父线程可 join */
static void *_conn_body(void *arg)
{
    app_tcp_client_conn_task(arg);
    return NULL;
}

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)attr;
    if (fn != app_tcp_client_conn_task) return NULL;
    if (pthread_create(&s_conn_thread, NULL, _conn_body, arg) != 0) return NULL;
    s_conn_thread_valid = true;
    return (osThreadId_t)(uintptr_t)&s_conn_thread;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static void fixture_reset(void)
{
    _reset_fakes();
    s_client_disconnect_sem = NULL;
    memset(s_host_ip, 0, sizeof(s_host_ip));
    s_host_ip[0] = 192;
    s_host_ip[1] = 168;
    s_host_ip[2] = 2;
    s_host_ip[3] = 17;
    s_host_port  = 9529;
    s_tcp_client_ccb.conn = NULL;
    s_tcp_client_ccb.base.state = APP_CCB_STATE_DOWN;
    g_tcp_client_task_handle = NULL;
}

static void _run_conn_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _conn_body, &s_conn) == 0, "conn 任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "conn 任务未退出");
}

static void *_client_thread_body(void *arg)
{
    app_tcp_client_task(arg);
    return NULL;
}

static void _run_client_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _client_thread_body, NULL) == 0, "主任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "主任务未退出");
    if (s_conn_thread_valid) {
        CHECK_MSG(pthread_join(s_conn_thread, NULL) == 0, "conn 任务未退出");
        s_conn_thread_valid = false;
    }
}

/* ================================================================ */

/** @brief 远端配置：默认值、set/get、改地址要打断当前连接（释放信号量） */
static void case_remote_config(void)
{
    TEST_BEGIN("远端配置：默认 192.168.2.17:9529；set_remote 改地址并打断当前连接");

    fixture_reset();

    CHECK_MSG(app_tcp_client_get_host_port() == 9529U, "默认端口应为 9529，得到 %u",
              (unsigned)app_tcp_client_get_host_port());
    const uint8_t *ip = app_tcp_client_get_host_ip();
    CHECK_MSG(ip[0] == 192 && ip[1] == 168 && ip[2] == 2 && ip[3] == 17,
              "默认地址应为 192.168.2.17，得到 %u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1],
              (unsigned)ip[2], (unsigned)ip[3]);

    const uint8_t new_ip[4] = {10, 1, 2, 3};
    app_tcp_client_set_remote(new_ip, 6001U);
    CHECK_MSG(app_tcp_client_get_host_port() == 6001U && memcmp(app_tcp_client_get_host_ip(),
                                                                 new_ip, 4) == 0,
              "set_remote 后 get 应返回新地址/端口");

    /* 有信号量时改地址 → 打断当前连接（主任务据此重连） */
    s_client_disconnect_sem = osSemaphoreNew(1, 0, NULL);
    app_tcp_client_set_remote(new_ip, 6002U);
    CHECK_MSG(osSemaphoreAcquire(s_client_disconnect_sem, 0) == osOK,
              "set_remote 有信号量时必须释放它（打断当前连接）");
}

/** @brief conn 任务：UP + keepalive + 只派发 len>1；断开置 DOWN、清 conn、放信号量 */
static void case_conn_lifecycle(void)
{
    TEST_BEGIN("conn 任务：UP/keepalive/派发；断开置 DOWN 并放信号量");

    fixture_reset();
    s_client_disconnect_sem = osSemaphoreNew(1, 0, NULL);

    _run_conn_task();

    CHECK_MSG((s_pcb.so_options & SOF_KEEPALIVE) != 0, "连接必须开 SOF_KEEPALIVE（%02X）",
              (unsigned)s_pcb.so_options);
    CHECK_MSG(s_pcb.keep_idle == 10000U && s_pcb.keep_intvl == 2000U && s_pcb.keep_cnt == 3U,
              "keepalive 应为 10s/2s×3（%u/%u/%u）", (unsigned)s_pcb.keep_idle,
              (unsigned)s_pcb.keep_intvl, (unsigned)s_pcb.keep_cnt);
    CHECK_MSG(s_disp_cnt == 1 && s_disp_len[0] == 8, "应派发一次 8 字节（%d/%u）", s_disp_cnt,
              (unsigned)s_disp_len[0]);
    CHECK_MSG(s_tcp_client_ccb.base.state == APP_CCB_STATE_DOWN && s_tcp_client_ccb.conn == NULL,
              "断开后必须置 DOWN 并清 conn（state=%d）", (int)s_tcp_client_ccb.base.state);
    CHECK_MSG(osSemaphoreAcquire(s_client_disconnect_sem, 0) == osOK,
              "断开后必须释放 disconnect_sem（否则主任务不再重连）");
}

/** @brief 建连成功：非阻塞 connect → 轮询到已连接 → 恢复阻塞 → conn 任务 → 重连循环 */
static void case_connect_success(void)
{
    TEST_BEGIN("建连成功：非阻塞 connect → 恢复阻塞 → 派生 conn 任务 → 断开重连");

    fixture_reset();
    _run_client_task();

    CHECK_MSG(s_connect_cnt == 1, "应 connect 一次（%d）", s_connect_cnt);
    CHECK_MSG(s_connect_port == 9529U, "connect 端口应为 9529，得到 %u",
              (unsigned)s_connect_port);
    CHECK_MSG(IP_GET_TYPE(&s_connect_addr) == IPADDR_TYPE_V4,
              "connect 地址应为 IPv4（类型 %d）", (int)IP_GET_TYPE(&s_connect_addr));
    const ip4_addr_t *a = ip_2_ip4(&s_connect_addr);
    CHECK_MSG(ip4_addr1(a) == 192 && ip4_addr2(a) == 168 && ip4_addr3(a) == 2 &&
                  ip4_addr4(a) == 17,
              "connect 地址应为 192.168.2.17（%u.%u.%u.%u）", (unsigned)ip4_addr1(a),
              (unsigned)ip4_addr2(a), (unsigned)ip4_addr3(a), (unsigned)ip4_addr4(a));
    CHECK_MSG(s_connect_nb == 1, "connect 时必须先置非阻塞（nb=%u）", (unsigned)s_connect_nb);
    CHECK_MSG(s_recv_nb == 0, "建连后必须恢复阻塞再 recv（nb=%u）", (unsigned)s_recv_nb);
    CHECK_MSG(s_close_cnt == 1 && s_delete_cnt == 1, "连接任务结束后应 close+delete 各 1 次（%d/%d）",
              s_close_cnt, s_delete_cnt);
    CHECK_MSG(s_disp_cnt == 1, "连接期间应派发 1 次（%d）", s_disp_cnt);
}

/** @brief 建连失败：删除 netconn、歇 1 秒再试，不得派生 conn 任务 */
static void case_connect_fail(void)
{
    TEST_BEGIN("建连失败：delete + 1s 重试，不派生 conn 任务");

    fixture_reset();
    s_connect_ret = ERR_RTE;

    _run_client_task();

    CHECK_MSG(s_connect_cnt == 1, "应尝试 connect 一次（%d）", s_connect_cnt);
    CHECK_MSG(s_delete_cnt == 1, "失败的 netconn 必须 delete（%d 次）", s_delete_cnt);
    CHECK_MSG(!s_conn_thread_valid && s_disp_cnt == 0, "失败不得派生 conn 任务/派发");
    CHECK_MSG(s_delay_cnt >= 1 && s_delay_ms == 1000U, "失败后应歇 1000ms 重试（%d 次，末次 %ums）",
              s_delay_cnt, (unsigned)s_delay_ms);
}

/** @brief 连接超时：4 秒内 state 一直是 CONNECT → 放弃并 delete，不派生 conn 任务 */
static void case_connect_timeout(void)
{
    TEST_BEGIN("连接超时：4s 内仍是 CONNECT → 放弃、delete、不派生 conn 任务");

    fixture_reset();
    s_connect_ret     = ERR_INPROGRESS;
    s_stay_connecting = true;
    s_conn.pcb.tcp    = &s_pcb;

    _run_client_task();

    CHECK_MSG(s_connect_cnt == 1, "应尝试 connect 一次（%d）", s_connect_cnt);
    CHECK_MSG(!s_conn_thread_valid && s_disp_cnt == 0, "超时不得派生 conn 任务/派发");
    CHECK_MSG(s_delete_cnt == 1, "超时的 netconn 必须 delete（%d 次）", s_delete_cnt);
    CHECK_MSG(s_tick >= 4000U, "轮询应走满 4 秒（tick=%u）", (unsigned)s_tick);
    CHECK_MSG(s_delay_500_cnt == 8, "超时轮询应按 500ms 步进 8 次（实际 %d 次）",
              s_delay_500_cnt);
}

/* ================================================================ */

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36mTCP 客户端通道（远端配置 / 超时 / conn 生命周期 / 重连）\033[0m\n");

    case_remote_config();
    case_conn_lifecycle();
    case_connect_success();
    case_connect_fail();
    case_connect_timeout();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
