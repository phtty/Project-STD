/**
 * @file    test_udp.c
 * @brief   UDP 通道：端口配置 / 广播发送 / 本帧来源回包 / 收包生命周期 / bind 循环
 *
 * UDP 是 IAP 搜索与应答的通道（端口 10011）。这里的现场症状是：
 *   - 广播开不了（`SOF_BROADCAST` 没置）→ 上位机搜不到设备；
 *   - 回包目标没取"本帧来源"→ 应答发到 0.0.0.0 或上一次的来源；
 *   - bind 失败后不 delete/不歇 → netconn 泄漏、越试越坏。
 *
 * 手法与 `test_tcp_server.c` 相同：真 LwIP 头文件 + 脚本化 netconn，
 * 任务跑在 pthread 上，无限循环由脚本里的 `pthread_exit()` 收尾。
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
#include "app_udp.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

/* ================================================================
 *  断言（变体 B）
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
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似 UDP 任务死等）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* `IP_ADDR_ANY` 指向 LwIP 的全局常量（真机在 ip_addr.c，socket 层不链接它） */
const ip_addr_t ip_addr_any = IPADDR4_INIT(0);

/* ================================================================
 *  RTOS 原语
 * ================================================================ */

typedef struct {
    sem_t sem;
} _sem_t;

static uint32_t s_tick;
static int      s_delay_cnt;
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
    if (timeout == osWaitForever) return (sem_wait(&s->sem) == 0) ? osOK : osError;
    if (timeout == 0U) return (sem_trywait(&s->sem) == 0) ? osOK : osErrorTimeout;

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
 * ================================================================ */

#include "../../Application/Src/Channel/app_udp.c"

/* ================================================================
 *  netconn / netbuf 脚本替身
 * ================================================================ */

static struct netconn s_conn;
static struct udp_pcb s_udp_pcb;
static struct netbuf  s_rx_buf;
static struct netbuf  s_tx_buf;

static int s_new_cnt;
static int s_bind_cnt;
static int s_delete_cnt;
static int s_recv_cnt;
static int s_sendto_cnt;
static int s_buf_new_cnt;
static int s_buf_del_cnt;

static bool     s_new_fail;
static err_t    s_bind_ret = ERR_OK;
static err_t    s_sendto_ret = ERR_OK;
static uint16_t s_bind_port;
static struct netconn *s_bind_conn[4];
static uint16_t s_sendto_port;
static uint32_t s_sendto_ip;
static uint16_t s_ref_len;
static const void *s_ref_data;

static int32_t  s_disp_cnt;
static uint16_t s_disp_len[8];

static ip_addr_t s_from_addr;
static uint16_t  s_from_port;

static pthread_t s_conn_thread;
static bool      s_conn_thread_valid;

static void _reset_fakes(void)
{
    memset(&s_conn, 0, sizeof(s_conn));
    memset(&s_udp_pcb, 0, sizeof(s_udp_pcb));
    memset(&s_rx_buf, 0, sizeof(s_rx_buf));
    memset(&s_tx_buf, 0, sizeof(s_tx_buf));
    s_conn.pcb.udp = &s_udp_pcb;

    s_new_cnt = s_bind_cnt = s_delete_cnt = s_recv_cnt = 0;
    s_sendto_cnt = s_buf_new_cnt = s_buf_del_cnt = 0;
    s_new_fail   = false;
    s_bind_ret   = ERR_OK;
    s_sendto_ret = ERR_OK;
    s_bind_port  = 0;
    memset(s_bind_conn, 0, sizeof(s_bind_conn));
    s_sendto_port = 0;
    s_sendto_ip   = 0;
    s_ref_len     = 0;
    s_ref_data    = NULL;
    s_disp_cnt    = 0;
    memset(s_disp_len, 0, sizeof(s_disp_len));

    IP4_ADDR(&s_from_addr, 10, 9, 8, 7);
    s_from_port = 1234;

    s_conn_thread_valid = false;
    s_tick              = 0;
    s_delay_cnt         = 0;
    s_delay_ms          = 0;
}

struct netconn *netconn_new_with_proto_and_callback(enum netconn_type type, u8_t proto,
                                                    netconn_callback callback)
{
    (void)proto;
    (void)callback;
    s_new_cnt++;
    if (s_new_fail) return NULL;
    if (s_new_cnt == 1) {
        s_conn.type = type;
        return &s_conn;
    }
    /* 脚本结束：任务循环在下一轮 new 处退出 */
    pthread_exit(NULL);
    return NULL;
}

err_t netconn_bind(struct netconn *conn, const ip_addr_t *addr, u16_t port)
{
    (void)addr;
    s_bind_cnt++;
    s_bind_port = port;
    if (s_bind_cnt <= (int)(sizeof(s_bind_conn) / sizeof(s_bind_conn[0])))
        s_bind_conn[s_bind_cnt - 1] = conn;
    /* 生产循环在 delete 之后**复用同一个 conn 指针**（见 docs §10 O6）；
       这里在第 2 次 bind 处退出线程，避免测试跟着走进已释放的 netconn。 */
    if (s_bind_cnt >= 2) pthread_exit(NULL);
    return s_bind_ret;
}

err_t netconn_delete(struct netconn *conn)
{
    (void)conn;
    s_delete_cnt++;
    return ERR_OK;
}

err_t netconn_recv(struct netconn *conn, struct netbuf **new_buf)
{
    (void)conn;
    s_recv_cnt++;
    if (s_recv_cnt == 1) {
        s_rx_buf.addr = s_from_addr; /* netbuf_fromaddr 是宏，直接填字段 */
        s_rx_buf.port = s_from_port;
        *new_buf      = &s_rx_buf;
        return ERR_OK;
    }
    return ERR_CLSD;
}

struct netbuf *netbuf_new(void)
{
    s_buf_new_cnt++;
    return &s_tx_buf;
}

err_t netbuf_ref(struct netbuf *buf, const void *data, u16_t size)
{
    (void)buf;
    s_ref_data = data;
    s_ref_len  = size;
    return ERR_OK;
}

void netbuf_delete(struct netbuf *buf) { (void)buf; s_buf_del_cnt++; }

err_t netconn_sendto(struct netconn *conn, struct netbuf *buf, const ip_addr_t *addr, u16_t port)
{
    (void)conn;
    (void)buf;
    s_sendto_cnt++;
    s_sendto_port = port;
    s_sendto_ip   = ip4_addr_get_u32(ip_2_ip4(addr));
    return s_sendto_ret;
}

/* 接收脚本：一包两段 [12, 0]（0 字节段必须跳过） */
static int s_rx_seg;

err_t netbuf_data(struct netbuf *buf, void **data, u16_t *len)
{
    (void)buf;
    static const uint8_t seg[2][16] = {{0}};
    *data = (void *)seg[s_rx_seg];
    *len  = (s_rx_seg == 0) ? 12U : 0U;
    return ERR_OK;
}

s8_t netbuf_next(struct netbuf *buf)
{
    (void)buf;
    s_rx_seg++;
    return (s8_t)((s_rx_seg < 2) ? 0 : -1);
}

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

/* conn 任务真的跑 pthread */
static void *_conn_body(void *arg)
{
    app_udp_connect_task(arg);
    return NULL;
}

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)attr;
    if (fn != app_udp_connect_task) return NULL;
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
    s_udp_disconnect_sem = NULL;
    s_udp_port           = 10011;
    s_udp_ccb.conn       = NULL;
    s_udp_ccb.base.state = APP_CCB_STATE_DOWN;
    memset(s_udp_ccb.src_ip, 0, sizeof(s_udp_ccb.src_ip));
    s_udp_ccb.src_port    = 0;
    s_rx_seg              = 0;
    g_udp_task_handle     = NULL;
    s_rx_count            = 0;
    s_tx_count            = 0;
}

static void _run_conn_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _conn_body, &s_conn) == 0, "conn 任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "conn 任务未退出");
}

static void *_udp_thread_body(void *arg)
{
    app_udp_task(arg);
    return NULL;
}

static void _run_udp_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _udp_thread_body, NULL) == 0, "UDP 任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "UDP 任务未退出");
    if (s_conn_thread_valid) {
        CHECK_MSG(pthread_join(s_conn_thread, NULL) == 0, "conn 任务未退出");
        s_conn_thread_valid = false;
    }
}

/* ================================================================ */

/** @brief 端口配置：默认 10011（IAP 搜索），set/get 一致 */
static void case_config(void)
{
    TEST_BEGIN("端口配置：默认 10011，set/get 一致");

    fixture_reset();
    CHECK_MSG(app_udp_get_port() == 10011U, "默认端口应为 10011，得到 %u",
              (unsigned)app_udp_get_port());

    app_udp_set_port(6002U);
    CHECK_MSG(app_udp_get_port() == 6002U, "set_port 后应返回 6002，得到 %u",
              (unsigned)app_udp_get_port());
}

/** @brief 广播：必须开 SOF_BROADCAST、发到 255.255.255.255:本端口，且 netbuf 不泄漏 */
static void case_broadcast(void)
{
    TEST_BEGIN("广播：SOF_BROADCAST + 目的 255.255.255.255，netbuf/conn 都回收");

    fixture_reset();
    const uint8_t payload[5] = {1, 2, 3, 4, 5};

    app_udp_broadcast(payload, sizeof(payload));

    CHECK_MSG(s_new_cnt == 1, "广播应新建 1 个 UDP netconn（%d）", s_new_cnt);
    CHECK_MSG((s_udp_pcb.so_options & SOF_BROADCAST) != 0, "广播必须置 SOF_BROADCAST（%02X）",
              (unsigned)s_udp_pcb.so_options);
    CHECK_MSG(s_ref_data == payload && s_ref_len == sizeof(payload),
              "netbuf_ref 应引用整段载荷（%p/%u）", (const void *)s_ref_data,
              (unsigned)s_ref_len);
    CHECK_MSG(s_sendto_cnt == 1 && s_sendto_port == 10011U, "应发到 10011（%d/%u）",
              s_sendto_cnt, (unsigned)s_sendto_port);
    CHECK_MSG(s_sendto_ip == 0xFFFFFFFFU, "目的地址应为 255.255.255.255，得到 %08X",
              (unsigned)s_sendto_ip);
    CHECK_MSG(s_buf_del_cnt == 1 && s_delete_cnt == 1, "netbuf 与 conn 都必须回收（%d/%d）",
              s_buf_del_cnt, s_delete_cnt);

    /* 新建失败：直接返回，不发、不崩 */
    fixture_reset();
    s_new_fail = true;
    app_udp_broadcast(payload, sizeof(payload));
    CHECK_MSG(s_sendto_cnt == 0 && s_buf_new_cnt == 0, "netconn 新建失败应直接返回");
}

/** @brief 发送门禁：DOWN/无 conn 丢弃；回包目标取本帧来源，广播按 dst 翻译 */
static void case_send(void)
{
    TEST_BEGIN("发送：DOWN 丢弃；回包取本帧来源；dst->broadcast 走全网广播");

    fixture_reset();
    uint8_t data[3] = {1, 2, 3};

    s_udp_ccb.conn = &s_conn; /* DOWN 但 conn 还挂着（刚断开、conn 尚未清）也必须丢弃 */
    CHECK_MSG(s_udp_ccb.base.ops->send(&s_udp_ccb.base, NULL, data, sizeof(data)) == -1,
              "DOWN 状态 send 必须丢弃");
    CHECK_MSG(s_sendto_cnt == 0, "DOWN 状态不得发送（%d）", s_sendto_cnt);

    s_udp_ccb.base.state = APP_CCB_STATE_UP;
    s_udp_ccb.conn       = &s_conn;
    s_udp_ccb.src_ip[0]  = 10;
    s_udp_ccb.src_ip[1]  = 1;
    s_udp_ccb.src_ip[2]  = 2;
    s_udp_ccb.src_ip[3]  = 3;
    s_udp_ccb.src_port   = 4567;

    CHECK_MSG(s_udp_ccb.base.ops->send(&s_udp_ccb.base, NULL, data, sizeof(data)) == 3,
              "UP 时 send 应返回字节数");
    ip4_addr_t expect;
    IP4_ADDR(&expect, 10, 1, 2, 3);
    CHECK_MSG(s_sendto_cnt == 1 && s_sendto_ip == ip4_addr_get_u32(&expect) &&
                  s_sendto_port == 4567U,
              "dst=NULL 应回本帧来源 10.1.2.3:4567（%08X/%u）", (unsigned)s_sendto_ip,
              (unsigned)s_sendto_port);
    CHECK_MSG(app_udp_get_tx_count() == 1, "成功发送应累加 tx 计数（%u）",
              (unsigned)app_udp_get_tx_count());

    const app_ccb_dst_t bcast = {.broadcast = true};
    CHECK_MSG(s_udp_ccb.base.ops->send(&s_udp_ccb.base, &bcast, data, sizeof(data)) == 3,
              "广播 dst 也应返回字节数");
    CHECK_MSG(s_sendto_ip == 0xFFFFFFFFU, "dst->broadcast 应发到 255.255.255.255（%08X）",
              (unsigned)s_sendto_ip);

    const uint32_t tx_before = app_udp_get_tx_count();
    s_sendto_ret             = ERR_MEM;
    CHECK_MSG(s_udp_ccb.base.ops->send(&s_udp_ccb.base, NULL, data, sizeof(data)) == -1,
              "sendto 失败时 send 应返回 -1");
    CHECK_MSG(app_udp_get_tx_count() == tx_before, "失败不得累加 tx 计数");
}

/** @brief 收包：len>0 才派发并记来源；len=0 段跳过；断开置 DOWN 并释放信号量 */
static void case_conn_lifecycle(void)
{
    TEST_BEGIN("收包：记来源+计数+派发；0 字节段跳过；断开置 DOWN 放信号量");

    fixture_reset();
    s_udp_disconnect_sem = osSemaphoreNew(1, 0, NULL);

    _run_conn_task();

    CHECK_MSG(s_disp_cnt == 1 && s_disp_len[0] == 12, "应只派发 len>0 的那一段（%d/%u）",
              s_disp_cnt, (unsigned)s_disp_len[0]);
    CHECK_MSG(s_udp_ccb.src_ip[0] == 10 && s_udp_ccb.src_ip[1] == 9 &&
                  s_udp_ccb.src_ip[2] == 8 && s_udp_ccb.src_ip[3] == 7,
              "应记住本帧来源地址 10.9.8.7（%u.%u.%u.%u）", (unsigned)s_udp_ccb.src_ip[0],
              (unsigned)s_udp_ccb.src_ip[1], (unsigned)s_udp_ccb.src_ip[2],
              (unsigned)s_udp_ccb.src_ip[3]);
    CHECK_MSG(s_udp_ccb.src_port == 1234U, "应记住本帧来源端口 1234（%u）",
              (unsigned)s_udp_ccb.src_port);
    CHECK_MSG(app_udp_get_rx_count() == 1, "成功收包应累加 rx 计数（%u）",
              (unsigned)app_udp_get_rx_count());
    CHECK_MSG(s_udp_ccb.base.state == APP_CCB_STATE_DOWN && s_udp_ccb.conn == NULL,
              "断开后必须置 DOWN 并清 conn");
    CHECK_MSG(osSemaphoreAcquire(s_udp_disconnect_sem, 0) == osOK,
              "断开后必须释放 disconnect_sem");
}

/**
 * @brief 任务循环：bind 端口 + SOF_BROADCAST；断开后 delete + 2s 再重试
 *
 * **现状哨兵**：`app_udp_task` 的 `netconn_new` 在循环**外**，`netconn_delete` 之后
 * 下一轮仍拿同一个 `conn` 指针去 bind —— 真机上是 use-after-free（第一次 bind 失败
 * 或网络恢复重试时就会踩到）。本用例只钉"确实 delete 了、确实歇了 2s、确实会重试"，
 * 不把"复用已释放指针"当契约；该问题登记在 docs/测试约定.md §10 O6，等产品/协议
 * 决策后再改实现与期望（届时循环内应重新 netconn_new）。
 */
static void case_task_loop(void)
{
    TEST_BEGIN("任务循环：bind 端口 + SOF_BROADCAST，断开后 delete + 2s 重试");

    fixture_reset();
    _run_udp_task();

    CHECK_MSG(s_new_cnt == 1, "当前实现只 new 一次（重试复用的是同一个 conn）—— 见 §10 O6（%d）",
              s_new_cnt);
    CHECK_MSG(s_bind_cnt == 2 && s_bind_port == 10011U, "第一次 bind 10011，断开后重试第二次（%d/%u）",
              s_bind_cnt, (unsigned)s_bind_port);
    CHECK_MSG(s_bind_conn[0] == s_bind_conn[1], "现状：重试复用同一 conn 指针（§10 O6）");
    CHECK_MSG((s_udp_pcb.so_options & SOF_BROADCAST) != 0, "bind 前必须置 SOF_BROADCAST");
    CHECK_MSG(s_disp_cnt == 1, "连接期间应派发 1 次（%d）", s_disp_cnt);
    CHECK_MSG(s_delete_cnt >= 1, "conn 结束后必须 delete（%d）", s_delete_cnt);
    CHECK_MSG(s_delay_cnt >= 1 && s_delay_ms == 2000U, "重试前应歇 2000ms（%d 次，末次 %u）",
              s_delay_cnt, (unsigned)s_delay_ms);
}

/** @brief 新建失败：任务直接退出，不得 bind/空转 */
static void case_task_new_fail(void)
{
    TEST_BEGIN("netconn_new 失败：任务直接退出（不 bind/不空转）");

    fixture_reset();
    s_new_fail = true;
    _run_udp_task();

    CHECK_MSG(s_bind_cnt == 0, "new 失败不得 bind（%d）", s_bind_cnt);
    CHECK_MSG(s_disp_cnt == 0 && s_delete_cnt == 0, "new 失败不得派发/delete");
}

/* ================================================================ */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36mUDP 通道（端口 / 广播 / 来源回包 / 收包生命周期 / bind 循环）\033[0m\n");

    case_config();
    case_broadcast();
    case_send();
    case_conn_lifecycle();
    case_task_loop();
    case_task_new_fail();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
