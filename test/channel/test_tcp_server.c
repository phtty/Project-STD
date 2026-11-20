/**
 * @file    test_tcp_server.c
 * @brief   TCP 服务器通道：实例 API / 发送门禁 / 连接任务生命周期 / manage 循环
 *
 * `app_tcp_server.c` 是 LDI 与 AH/TCP 共用的一条通道，最近改过两处容易复发的地方：
 *   1. **实例化**（默认 9528 + 一个扩展实例）与"没绑协议就别开监听"；
 *   2. **conn 任务早退必须放 `disconnect_sem`**，否则 manage 任务永远等在这个连接上，
 *      监听口死掉 —— 现场表现是"UDP 还能搜到、TCP 怎么都连不上，只能断电"。
 *
 * 这两处都不会在协议套件里暴露（那边把通道桩掉了），所以这里直接跑真任务：
 * 用 LwIP 的真头文件（`struct netconn` / `struct tcp_pcb` 的字段就是 keepalive 契约），
 * 只把 netconn 的**行为**换成脚本（new/bind/listen/accept/recv/write/close/delete）。
 *
 * 无限循环的两个任务都靠脚本里的 `pthread_exit()` 退出（manage 的 netconn_new
 * 在脚本结束后退出、conn 任务本来就走 `osThreadExit()`），父线程 join 后断言。
 */

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_dispatch.h"
#include "app_tcp_server.h"
#include "pl_net_adapt.h"
#include "pl_task.h"

/* ================================================================
 *  断言（与 test_rls 同一变体 B，见 docs/测试约定.md 附录 B）
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
 *
 *  manage/conn 都是真 pthread；脚本写错时最容易的表现是"父线程 join 永远等"，
 *  用 SIGALRM 把挂死转成明确的失败退出（§1 挂死保护）。
 * ================================================================ */

#define TEST_TIMEOUT_S (20)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\n\033[31m✘ 测试超时（疑似 manage/conn 任务死等）\033[0m\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

/* ================================================================
 *  生产模块本体（TU-include：实例池、默认端口、conn 任务都是本文件可见的）
 * ================================================================ */

#include "../../Application/Src/Channel/app_tcp_server.c"

/* ================================================================
 *  RTOS 原语：本套件自己实现（os_stub 的无限等待不支持，而 manage 任务必须等到
 *  conn 任务断开才知道下一轮）。只实现 app_tcp_server.c 用到的五个。
 * ================================================================ */

typedef struct {
    sem_t sem;
} _sem_t;

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
    struct timespec ts = {.tv_sec = (time_t)(ticks / 1000U),
                          .tv_nsec = (long)(ticks % 1000U) * 1000000L};
    nanosleep(&ts, NULL);
    return osOK;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    (void)func;
    (void)argument;
    (void)attr;
    return NULL; /* 本套件不通过它起任务（只测已启动守卫与 proto_cnt==0 早退） */
}

void osThreadExit(void) { pthread_exit(NULL); }

/* ================================================================
 *  netconn 脚本替身
 * ================================================================ */

static struct netconn  s_listen_conn;
static struct netconn  s_client_conn;
static struct tcp_pcb  s_listen_pcb;
static struct tcp_pcb  s_client_pcb;
static struct netbuf   s_netbuf;

/* 调用记录 */
static int s_new_cnt;
static int s_bind_cnt;
static int s_listen_cnt;
static int s_accept_cnt;
static int s_delete_cnt;
static int s_close_cnt;
static int s_write_cnt;
static int s_recv_cnt;

static err_t    s_new_ret    = ERR_OK;
static err_t    s_bind_ret   = ERR_OK;
static err_t    s_listen_ret = ERR_OK;
static err_t    s_accept_ret = ERR_OK;
static err_t    s_write_ret  = ERR_OK;
static uint16_t s_bound_port;
static uint16_t s_written_len;

/* 接收脚本：一次 recv 里给 n 段，下一 recv 断开 */
typedef struct {
    const uint8_t *data;
    uint16_t       len;
} seg_t;

static seg_t   s_segs[4];
static int     s_seg_cnt;
static int     s_seg_idx;

/* 派发记录 */
static int      s_disp_cnt;
static uint16_t s_disp_len[8];
static uint8_t  s_disp_data[8][32];

/* conn 任务线程（pl_task_new 创建） */
static pthread_t s_conn_thread;
static bool      s_conn_thread_valid;

static void _reset_fakes(void)
{
    memset(&s_listen_conn, 0, sizeof(s_listen_conn));
    memset(&s_client_conn, 0, sizeof(s_client_conn));
    memset(&s_listen_pcb, 0, sizeof(s_listen_pcb));
    memset(&s_client_pcb, 0, sizeof(s_client_pcb));
    memset(&s_netbuf, 0, sizeof(s_netbuf));
    s_listen_conn.pcb.tcp = &s_listen_pcb;
    s_client_conn.pcb.tcp = &s_client_pcb;

    s_new_cnt = s_bind_cnt = s_listen_cnt = s_accept_cnt = 0;
    s_delete_cnt = s_close_cnt = s_write_cnt = s_recv_cnt = 0;

    s_new_ret = s_bind_ret = s_listen_ret = s_accept_ret = ERR_OK;
    s_write_ret = ERR_OK;
    s_bound_port = 0;
    s_written_len = 0;

    s_seg_cnt = s_seg_idx = 0;
    memset(s_segs, 0, sizeof(s_segs));

    s_disp_cnt = 0;
    memset(s_disp_len, 0, sizeof(s_disp_len));
    memset(s_disp_data, 0, sizeof(s_disp_data));

    s_conn_thread_valid = false;
}

/* ---- netconn：行为全部脚本化 ---- */
struct netconn *netconn_new_with_proto_and_callback(enum netconn_type type, u8_t proto,
                                                    netconn_callback callback)
{
    (void)proto;
    (void)callback;
    s_new_cnt++;
    if (s_new_cnt == 1 && s_new_ret == ERR_OK) {
        s_listen_conn.type = type;
        return &s_listen_conn;
    }
    /* 脚本结束：manage 任务是无限循环，从这里退出线程（与真机 osThreadExit 等价） */
    pthread_exit(NULL);
    return NULL;
}

err_t netconn_bind(struct netconn *conn, const ip_addr_t *addr, uint16_t port)
{
    (void)conn;
    (void)addr;
    s_bind_cnt++;
    s_bound_port = port;
    return s_bind_ret;
}

err_t netconn_listen_with_backlog(struct netconn *conn, u8_t backlog)
{
    (void)conn;
    (void)backlog;
    s_listen_cnt++;
    return s_listen_ret;
}

err_t netconn_accept(struct netconn *conn, struct netconn **new_conn)
{
    (void)conn;
    s_accept_cnt++;
    if (s_accept_ret != ERR_OK) return s_accept_ret;
    *new_conn = &s_client_conn;
    return ERR_OK;
}

err_t netconn_delete(struct netconn *conn)
{
    (void)conn;
    s_delete_cnt++;
    return ERR_OK;
}

err_t netconn_write_partly(struct netconn *conn, const void *data, size_t size, u8_t apiflags,
                           size_t *bytes_written)
{
    (void)conn;
    (void)data;
    (void)apiflags;
    s_write_cnt++;
    s_written_len = (uint16_t)size;
    if (bytes_written != NULL) *bytes_written = size;
    return s_write_ret;
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
    /* 第一次给一整包（多段），之后断开 */
    if (s_recv_cnt == 1) {
        s_seg_idx = 0;
        *new_buf  = &s_netbuf;
        return ERR_OK;
    }
    return ERR_CLSD;
}

err_t netbuf_data(struct netbuf *buf, void **data, u16_t *len)
{
    (void)buf;
    *data = (void *)s_segs[s_seg_idx].data;
    *len  = s_segs[s_seg_idx].len;
    return ERR_OK;
}

s8_t netbuf_next(struct netbuf *buf)
{
    (void)buf;
    s_seg_idx++;
    return (s8_t)((s_seg_idx < s_seg_cnt) ? 0 : -1);
}

void netbuf_delete(struct netbuf *buf) { (void)buf; }

/* ---- 派发引擎：只记录，不判断 ---- */
void app_ccb_dispatch(const app_ccb_t *ccb, const app_ccb_src_t *src, const uint8_t *data,
                      uint16_t len)
{
    (void)ccb;
    (void)src;
    if (s_disp_cnt < (int)(sizeof(s_disp_len) / sizeof(s_disp_len[0]))) {
        s_disp_len[s_disp_cnt] = len;
        if (len <= sizeof(s_disp_data[0])) memcpy(s_disp_data[s_disp_cnt], data, len);
        s_disp_cnt++;
    }
}

/* ---- 任务创建：真的起 pthread，父线程可 join ---- */
static void *_conn_thread_body(void *arg)
{
    app_tcp_server_conn_task(arg);
    return NULL;
}

osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)attr;
    if (fn != app_tcp_server_conn_task) return NULL;
    if (pthread_create(&s_conn_thread, NULL, _conn_thread_body, arg) != 0) return NULL;
    s_conn_thread_valid = true;
    return (osThreadId_t)(uintptr_t)&s_conn_thread;
}

/* ================================================================
 *  夹具
 * ================================================================ */

static app_tcp_server_t s_srv;

/** @brief 清空实例池与脚本（每个用例开头调一次） */
static void fixture_reset(void)
{
    memset(&s_default_srv, 0, sizeof(s_default_srv));
    memset(&s_extra_srv, 0, sizeof(s_extra_srv));
    s_default_created = false;
    s_extra_used      = false;
    s_default_port    = 9528U;
    g_tcp_server_task_handle = NULL;

    memset(&s_srv, 0, sizeof(s_srv));
    _reset_fakes();
}

/** @brief 摆一个已就绪的实例（信号量 + 连接），供 conn 任务用例直接调用 */
static void _srv_ready(uint32_t idle_timeout_ms)
{
    _server_init(&s_srv, "ut_srv", 9528U, idle_timeout_ms);
    s_srv.disconnect_sem = osSemaphoreNew(1, 0, NULL);
}

static void _run_conn_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _conn_thread_body, &s_srv) == 0, "conn 任务线程创建失败");
    CHECK_MSG(pthread_join(tid, NULL) == 0, "conn 任务未退出");
}

static void *_manage_thread_body(void *arg)
{
    app_tcp_server_task(arg);
    return NULL;
}

static void _run_manage_task(void)
{
    pthread_t tid;
    CHECK_MSG(pthread_create(&tid, NULL, _manage_thread_body, &s_srv) == 0,
              "manage 任务线程创建失败");
    /* manage 线程在脚本结束后自行退出 */
    CHECK_MSG(pthread_join(tid, NULL) == 0, "manage 任务未退出");
    if (s_conn_thread_valid) {
        CHECK_MSG(pthread_join(s_conn_thread, NULL) == 0, "conn 任务未退出");
        s_conn_thread_valid = false;
    }
}

static void _add_seg(const uint8_t *data, uint16_t len)
{
    s_segs[s_seg_cnt].data = data;
    s_segs[s_seg_cnt].len  = len;
    s_seg_cnt++;
}

/* ================================================================ */

/** @brief 实例 API 与默认实例：端口、名字、空指针、扩展实例只允许一个 */
static void case_instance_api(void)
{
    fixture_reset();

    app_ccb_t *d = app_tcp_server_ccb();
    CHECK_MSG(d != NULL && strcmp(d->name, "tcp_server") == 0, "默认实例名应为 tcp_server（%s）",
              d ? d->name : "(null)");
    CHECK_MSG(d != NULL && d->state == APP_CCB_STATE_DOWN, "默认实例初始应为 DOWN（state=%d）",
              d ? (int)d->state : -1);
    CHECK_MSG(app_tcp_server_get_port() == 9528U, "默认端口应为 9528，得到 %u",
              (unsigned)app_tcp_server_get_port());

    app_tcp_server_set_port(7000U);
    CHECK_MSG(app_tcp_server_get_port() == 7000U, "set_port 后 get 应返回 7000，得到 %u",
              (unsigned)app_tcp_server_get_port());
    CHECK_MSG(app_tcp_server_ccb() == d && d->name != NULL, "重复取默认实例应返回同一实例");

    CHECK_MSG(app_tcp_server_open(NULL, 6001U, 0U) == NULL, "空名字必须被拒");
    CHECK_MSG(app_tcp_server_open("ah", 0U, 0U) == NULL, "端口 0 必须被拒");

    app_tcp_server_t *extra = app_tcp_server_open("ah", 6001U, 3000U);
    CHECK_MSG(extra != NULL, "第一个扩展实例应创建成功");
    CHECK_MSG(app_tcp_server_open("ah2", 6002U, 0U) == NULL, "扩展实例只允许一个");
    CHECK_MSG(app_tcp_server_ccb_of(extra) != NULL && strcmp(app_tcp_server_ccb_of(extra)->name, "ah") == 0,
              "扩展实例的 ccb 应可取出且名字为 ah");
    CHECK_MSG(app_tcp_server_ccb_of(NULL) == NULL, "ccb_of(NULL) 应返回 NULL");

    /* 没绑协议：默认实例不得开监听任务（否则白占 netconn + 1KB 栈） */
    CHECK_MSG(app_tcp_server_start() == NULL, "proto_cnt==0 时 start 不得创建任务");
    CHECK_MSG(app_tcp_server_start_instance(NULL) == NULL, "start_instance(NULL) 应返回 NULL");

    /* 已启动的实例重复 start 只返回旧句柄，不再起第二个任务 */
    app_tcp_server_set_port(9528U);
    app_tcp_server_t *dflt = (app_tcp_server_t *)d; /* ccb 是首成员，base 又是 ccb 首成员 */
    dflt->started     = true;
    dflt->task_handle = (osThreadId_t)(uintptr_t)0x1234U;
    CHECK_MSG(app_tcp_server_start_instance(dflt) == (osThreadId_t)(uintptr_t)0x1234U,
              "重复启动应返回已存在的任务句柄");
}

/** @brief 发送门禁：DOWN / conn 为空一律丢弃；UP 才调 netconn_write，失败返回 -1 */
static void case_send(void)
{
    fixture_reset();
    _srv_ready(0U);

    uint8_t data[4] = {1, 2, 3, 4};

    CHECK_MSG(s_srv.ccb.base.ops != NULL && s_srv.ccb.base.ops->send != NULL, "ccb 没挂 send 虚表");
    CHECK_MSG(s_srv.ccb.base.ops->send(&s_srv.ccb.base, NULL, data, sizeof(data)) == -1,
              "DOWN 状态 send 必须丢弃（返回 -1）");
    CHECK_MSG(s_write_cnt == 0, "DOWN 状态不得写 netconn（调用 %d 次）", s_write_cnt);

    /* DOWN 但 conn 还挂着（刚断开、conn 尚未清）：仍必须丢弃 —— 这条防的是
       "只查 conn 不查 state" 的写法把数据写进正在关闭的连接 */
    s_srv.ccb.conn = &s_client_conn;
    CHECK_MSG(s_srv.ccb.base.ops->send(&s_srv.ccb.base, NULL, data, sizeof(data)) == -1,
              "DOWN + 有 conn 也必须丢弃（返回 -1）");
    CHECK_MSG(s_write_cnt == 0, "DOWN + 有 conn 也不得写 netconn（调用 %d 次）", s_write_cnt);
    s_srv.ccb.conn = NULL;

    s_srv.ccb.base.state = APP_CCB_STATE_UP;
    CHECK_MSG(s_srv.ccb.base.ops->send(&s_srv.ccb.base, NULL, data, sizeof(data)) == -1,
              "conn 为空时 send 必须丢弃（返回 -1）");

    s_srv.ccb.conn = &s_client_conn;
    CHECK_MSG(s_srv.ccb.base.ops->send(&s_srv.ccb.base, NULL, data, sizeof(data)) == 4,
              "UP + conn 时 send 应返回写入字节数");
    CHECK_MSG(s_write_cnt == 1 && s_written_len == 4, "应写 1 次 4 字节（%d/%u）", s_write_cnt,
              (unsigned)s_written_len);

    s_write_ret = ERR_MEM;
    CHECK_MSG(s_srv.ccb.base.ops->send(&s_srv.ccb.base, NULL, data, sizeof(data)) == -1,
              "netconn_write 失败时 send 应返回 -1");
}

/** @brief 连接任务：keepalive 必须开、只派发 len>1 的段、断开后 DOWN 并释放信号量 */
static void case_conn_lifecycle(void)
{
    TEST_BEGIN("连接任务：keepalive / 多段派发 / 断开置 DOWN 并释放信号量");

    fixture_reset();
    _srv_ready(0U);
    s_srv.ccb.conn      = &s_client_conn;
    s_srv.ccb.base.state = APP_CCB_STATE_DOWN;

    static const uint8_t d10[10] = {0};
    static const uint8_t d1[1]   = {0};
    static const uint8_t d5[5]   = {0};
    _add_seg(d10, sizeof(d10));
    _add_seg(d1, sizeof(d1)); /* 单字节段：不派发 */
    _add_seg(d5, sizeof(d5));

    _run_conn_task();

    CHECK_MSG((s_client_pcb.so_options & SOF_KEEPALIVE) != 0,
              "连接必须开 SOF_KEEPALIVE（so_options=%02X）", (unsigned)s_client_pcb.so_options);
    CHECK_MSG(s_client_pcb.keep_idle == 10000U && s_client_pcb.keep_intvl == 2000U &&
                  s_client_pcb.keep_cnt == 3U,
              "keepalive 参数应为 10s/2s×3（%u/%u/%u）", (unsigned)s_client_pcb.keep_idle,
              (unsigned)s_client_pcb.keep_intvl, (unsigned)s_client_pcb.keep_cnt);
    CHECK_MSG(s_client_conn.recv_timeout == 0U, "未配置空闲超时时不得设 recvtimeout（%u）",
              (unsigned)s_client_conn.recv_timeout);
    CHECK_MSG(s_disp_cnt == 2 && s_disp_len[0] == 10 && s_disp_len[1] == 5,
              "只派发 len>1 的段（派发 %d 次，长度 %u/%u）", s_disp_cnt,
              (unsigned)s_disp_len[0], (unsigned)s_disp_len[1]);
    CHECK_MSG(s_srv.ccb.base.state == APP_CCB_STATE_DOWN && s_srv.ccb.conn == NULL,
              "断开后必须置 DOWN 并清 conn（state=%d）", (int)s_srv.ccb.base.state);
    CHECK_MSG(s_close_cnt == 1 && s_delete_cnt == 1, "断开应 close+delete 各 1 次（%d/%d）",
              s_close_cnt, s_delete_cnt);
    CHECK_MSG(osSemaphoreAcquire(s_srv.disconnect_sem, 0) == osOK,
              "断开后必须释放 disconnect_sem（否则 manage 永远等不到下一个连接）");
}

/** @brief 早退路径：conn 为空也要放信号量，否则监听口死掉 */
static void case_conn_early_exit(void)
{
    TEST_BEGIN("连接任务早退：conn 为空也必须释放信号量");

    fixture_reset();
    _srv_ready(0U);
    s_srv.ccb.conn = NULL;

    _run_conn_task();

    CHECK_MSG(osSemaphoreAcquire(s_srv.disconnect_sem, 0) == osOK,
              "早退必须释放 disconnect_sem");
    CHECK_MSG(s_disp_cnt == 0 && s_close_cnt == 0 && s_delete_cnt == 0,
              "早退不得派发/关闭（disp=%d close=%d del=%d）", s_disp_cnt, s_close_cnt,
              s_delete_cnt);
}

/** @brief 显式配了空闲超时才设 recvtimeout（默认实例保持无限等待 + keepalive） */
static void case_conn_idle_timeout(void)
{
    TEST_BEGIN("连接任务：只有配置了空闲超时才设 recvtimeout");

    fixture_reset();
    _srv_ready(15000U);
    s_srv.ccb.conn = &s_client_conn;

    _run_conn_task();

    CHECK_MSG(s_client_conn.recv_timeout == 15000U, "recvtimeout 应为配置的 15000ms，得到 %u",
              (unsigned)s_client_conn.recv_timeout);
    CHECK_MSG((s_client_pcb.so_options & SOF_KEEPALIVE) != 0, "配了空闲超时也要开 keepalive");
}

/** @brief manage 循环：bind/listen/accept → 派生 conn → 等断开 → 重新 listen */
static void case_manage_loop(void)
{
    TEST_BEGIN("manage 循环：bind/listen/accept → conn 任务 → 等断开 → 下一轮");

    fixture_reset();
    _srv_ready(0U);

    static const uint8_t d8[8] = {0};
    _add_seg(d8, sizeof(d8));

    _run_manage_task();

    CHECK_MSG(s_bind_cnt == 1 && s_bound_port == 9528U, "应 bind 一次默认端口（%d/%u）",
              s_bind_cnt, (unsigned)s_bound_port);
    CHECK_MSG(s_listen_cnt == 1, "bind 成功后应 listen（%d 次）", s_listen_cnt);
    CHECK_MSG(s_accept_cnt == 1, "应 accept 一次（%d 次）", s_accept_cnt);
    CHECK_MSG(s_delete_cnt >= 2, "监听 conn 与客户端 conn 都应 delete（%d 次）", s_delete_cnt);
    CHECK_MSG(s_disp_cnt == 1 && s_disp_len[0] == 8, "连接期间应派发一次 8 字节（%d/%u）",
              s_disp_cnt, (unsigned)s_disp_len[0]);
    CHECK_MSG(s_close_cnt == 1, "客户端断开应 close 一次（%d）", s_close_cnt);
    CHECK_MSG(s_srv.ccb.base.state == APP_CCB_STATE_DOWN && s_srv.ccb.conn == NULL,
              "manage 收尾后状态应为 DOWN/无连接");
}

/** @brief bind 失败：delete 掉监听 conn、歇 500ms 后重来（不能空转/泄漏） */
static void case_manage_bind_fail(void)
{
    TEST_BEGIN("manage 循环：bind 失败要 delete + 延时重试");

    fixture_reset();
    _srv_ready(0U);
    s_bind_ret = ERR_USE;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    _run_manage_task();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    const long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    CHECK_MSG(s_bind_cnt == 1, "应尝试 bind 一次（%d）", s_bind_cnt);
    CHECK_MSG(s_listen_cnt == 0, "bind 失败不得 listen（%d 次）", s_listen_cnt);
    CHECK_MSG(s_delete_cnt == 1, "bind 失败的监听 conn 必须 delete（%d 次）", s_delete_cnt);
    CHECK_MSG(ms >= 450L && ms < 3000L, "bind 失败应歇 500ms 再重试，实测 %ldms", ms);
}

/* ================================================================ */

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(TEST_TIMEOUT_S);

    printf("\n\033[36mTCP 服务器通道（实例 / 发送 / conn 生命周期 / manage 循环）\033[0m\n");

    case_instance_api();
    case_send();
    case_conn_lifecycle();
    case_conn_early_exit();
    case_conn_idle_timeout();
    case_manage_loop();
    case_manage_bind_fail();

    alarm(0);
    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
