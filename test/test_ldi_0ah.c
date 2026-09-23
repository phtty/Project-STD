/**
 * @file    test_ldi_0ah.c
 * @brief   LDI 0AH（设备 IP 信息设置）跨两条记录的交互 —— host 单测
 *
 * 为什么需要这个测试：0AH 一次操作写**两条互不相干的记录** ——
 *
 *   · LDI 自己的配置，在 W25Qxx 尾部（走 app_cfg_sched → dev_cfg_record）
 *   · IAP 记录里的 net_cfg 镜像，在内部 Flash Sector 1（走 dev_flash_int）
 *
 * 两条记录各有测试（test_cfg_sched / test_iap_cfg），但**它们之间的关系**没有：
 * 谁先谁后、一个失败另一个还写不写、回执里的状态字节反映的是哪一条。
 * 这几个问题恰好是现场踩过的坑 —— 0AH 报"设置成功"而上位机重启后拿到旧 IP，
 * 就是因为回执取自 LDI 那条、而镜像那条没跟上。
 *
 * 本文件把三条性质钉住：
 *   1. 0AH 后两条记录都含新值
 *   2. LDI 那条写失败 → 回执 status=0x01，但 IAP 镜像**仍然被尝试更新**（互不阻塞）
 *   3. LDI 那条成功、镜像那条失败 → 回执仍为成功
 *      （**已知取舍**，见 app_ldi_cmd.c 的注释：回执只反映 LDI 那条。
 *       钉住它是为了它被改动时能显式暴露，而不是当它是正确行为。）
 *   4. 0AH 不改运行态 IP（用户 2026-09-18 定的"下次上电生效"语义）
 *
 * 被测代码是生产源码本体。三个实现 TU 直接 include（原因与 test_cfg_sched.c 一致：
 * 注册入口与另两个锁初始化都是 static、由 initcall 调用，host 上没有 initcall 段
 * 运行器，从外部无法触达）。Makefile 里因此**不能**再列这三个 .c。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app_cfg_sched.h"
#include "app_ldi.h"
#include "app_ldi_cfg.h"
#include "dev_cfg_record.h"
#include "dev_flash_int.h"

#include "../Application/Src/LDI/app_ldi_cfg.c" /* 静态注册入口 */
#include "../Application/Src/IAP/app_iap_cfg.c" /* _iap_cfg_lock_init */
#include "../Application/Src/LDI/app_ldi_cmd.c" /* _ldi_cmd_set_ip 与 cmd_set_ip_t（后者在 .c 内定义） */

/* pl_flash 替身的控制接口（test/stubs/pl_flash_stub.c） */
void pl_flash_stub_set_region(uint32_t base, uint32_t len);
void pl_flash_stub_reset(void);
void pl_flash_stub_fail_after(int n);
int  pl_flash_stub_erase_count(void);

/* ================================================================
 *  断言
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                          \
    } while (0)

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
 *  外发帧捕获 —— 替代 app_dispatch.c 的 app_ccb_send（本用例不测发送路径）
 * ================================================================ */

static uint8_t  s_tx_buf[512];
static uint16_t s_tx_len;
static int      s_tx_count;

int32_t app_ccb_send(app_ccb_t *ccb, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    s_tx_count++;
    s_tx_len = len <= sizeof(s_tx_buf) ? len : sizeof(s_tx_buf);
    if (data && s_tx_len) memcpy(s_tx_buf, data, s_tx_len);
    return (int32_t)len; /* 桩：装作发出去了 */
}

/** @brief 从捕获到的响应帧里取 status 字节
 *
 *  帧布局（_ldi_send_response）：stx(2) ver(1) seq(1) len(4) data[...] crc(2)。
 *  即 payload 从偏移 8 开始。**status 不是 payload 的首字节** ——
 *  ldi_status_rsp_t 是 { head; status; payload[] }，head 在前。
 *  （第一版就栽在这：直接读 s_tx_buf[8]，读到的 0xA0 是 head 里的命令码。） */
static int rsp_status(void)
{
    if (s_tx_count == 0) return -1;
    return s_tx_buf[8 + offsetof(ldi_status_rsp_t, status)];
}

/* ================================================================
 *  两块独立的假 Flash
 *
 *  · W25Qxx（LDI 记录）：普通 RAM 即可，cfg_sched 走的是偏移寻址
 *  · 内部 Flash（IAP 记录）：**必须落在 32 位可寻址区** ——
 *    dev_flash_int_t.base_addr 是 uint32_t，64 位宿主机上直接塞指针会被截断
 *    （同 test_iap_cfg.c 的说明）
 * ================================================================ */

#define FAKE_CAP (32U * 1024U * 1024U) /* 必须过 cfg_sched 的容量门槛 */
#define FAKE_SECTOR_SIZE 4096

static uint8_t      *s_w25;
static dev_storage_t s_w25_dev;
static uint8_t      *s_int;
static uint32_t      s_int_base;

static int32_t w25_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)d;
    if ((uint64_t)addr + len > FAKE_CAP) return -1;
    memcpy(buf, s_w25 + addr, len);
    return 0;
}

/* NOR 语义：只能把 1 写成 0；要改的位已经非 0xFF 就先擦整扇区（与
   dev_w25qxx._write 的 RMW 同构）。桩比真硬件宽容的话会掩盖真缺陷。 */
static bool s_w25_fail_write; /* 用例注入"W25Qxx 写失败" */

static int32_t w25_write(dev_storage_t *d, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)d;
    if (s_w25_fail_write) return -1;
    if ((uint64_t)addr + len > FAKE_CAP) return -1;
    for (uint32_t i = 0; i < len; i++)
        if ((s_w25[addr + i] & buf[i]) != buf[i]) {
            memset(s_w25 + (addr & ~(uint32_t)0xFFF), 0xFF, 4096);
            break;
        }
    for (uint32_t i = 0; i < len; i++) s_w25[addr + i] &= buf[i];
    return 0;
}

static int32_t w25_erase(dev_storage_t *d, uint32_t addr, uint32_t len)
{
    (void)d;
    (void)addr;
    (void)len;
    return 0;
}
static uint32_t w25_capacity(dev_storage_t *d)
{
    (void)d;
    return FAKE_CAP;
}

static const dev_storage_ops_t w25_ops = {
    .read = w25_read, .write = w25_write, .erase = w25_erase, .capacity = w25_capacity};

dev_storage_t *dev_w25qxx_get(void) { return &s_w25_dev; }

/* ---- 生产代码依赖、本用例不关心的接口 ---- */
void          pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memset(ip, 0, 4);
    memset(mask, 0, 4);
    memset(gw, 0, 4);
}
uint16_t app_tcp_server_get_port(void) { return 9529; }

/* ---- 其余 11 个命令的依赖 ----
 * g_ldi_cmd_table[] 是非 static 全局，实测在本 TU 里**没有**被 gc-sections 丢掉，
 * 于是 12 个处理函数全被拉进来，它们的依赖也得给全。都是本用例不关心的东西，
 * 给最小实现即可 —— 注意别在这里写"看起来合理"的逻辑，那会让别的用例产生假信心。 */
pl_rtc_handle_t pl_rtc_get_handle(void) { return (pl_rtc_handle_t)&s_tx_buf; }
uint32_t        pl_rtc_get_timestamp(pl_rtc_handle_t h)
{
    (void)h;
    return 0;
}
bool pl_rtc_set_timestamp(pl_rtc_handle_t h, uint32_t ts)
{
    (void)h;
    (void)ts;
    return true;
}
void pl_system_reset(void) {}
void app_vms_ctrl(app_ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    (void)ctx;
    (void)text_len;
}
void app_udp_broadcast(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}

/* 运行态改 IP 的入口。0AH **不应**调它（"下次上电生效"语义），用例据此断言。 */
static int s_set_ip_calls;
void       pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    (void)ip;
    (void)mask;
    (void)gw;
    s_set_ip_calls++;
}

/* ================================================================
 *  环境搭建
 * ================================================================ */

static void env_setup(void)
{
    /* W25Qxx：MAP_SHARED 不必（本文件不做 fork 模拟重启） */
    if (!s_w25) s_w25 = mmap(NULL, FAKE_CAP, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s_w25 == MAP_FAILED) { printf("w25 mmap 失败\n"); exit(2); }
    memset(s_w25, 0xFF, FAKE_CAP);
    s_w25_dev.ops      = &w25_ops;
    s_w25_dev.capacity = FAKE_CAP;

    /* 内部 Flash：必须 32 位可寻址 */
    if (!s_int) {
        void *p = mmap(NULL, FAKE_SECTOR_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) { printf("内部 Flash mmap(MAP_32BIT) 失败\n"); exit(2); }
        s_int = p;
    }
    memset(s_int, 0xFF, FAKE_SECTOR_SIZE);
    s_int_base = (uint32_t)(uintptr_t)s_int;

    dev_flash_int_t *st = (dev_flash_int_t *)app_flash_iap_get_storage();
    st->base_addr       = s_int_base;
    st->base.ops          = &g_flash_int_ops;
    g_iap_sys_info            = (app_flash_iap_sys_info_t *)(uintptr_t)s_int_base;

    pl_flash_stub_set_region(s_int_base, FAKE_SECTOR_SIZE);
    pl_flash_stub_reset();

    /* 补上固件里由 initcall 做的初始化（host 上不跑 initcall） */
    if (!s_lock) _iap_cfg_lock_init();
    _app_flash_ldi_cfg_register();

    /* g_ldi_ctx 来自 app_ldi.c —— 用真实的那一份，不自己造 */
    if (!g_ldi_ctx.tx_lock) {
        const osMutexAttr_t attr = {.name = "ldi_tx", .attr_bits = osMutexPrioInherit};
        g_ldi_ctx.tx_lock            = osMutexNew(&attr);
    }
    g_ldi_ctx.rsp_seq = 0x11;

    s_tx_count    = 0;
    s_tx_len      = 0;
    s_set_ip_calls = 0;
    s_w25_fail_write = false;
}

/** @brief 构造一条 0AH 请求载荷 */
static void make_0ah(cmd_set_ip_t *req, const uint8_t ip[4], const uint8_t mask[4],
                     const uint8_t gw[4], uint16_t port)
{
    memset(req, 0, sizeof(*req));
    memcpy(req->net.device_ip, ip, 4);
    memcpy(req->net.netmask, mask, 4);
    memcpy(req->net.gateway, gw, 4);
    req->net.device_port[0] = (uint8_t)(port >> 8);
    req->net.device_port[1] = (uint8_t)(port & 0xFF);
}

static const uint8_t IP_A[4]   = {10, 20, 30, 40};
static const uint8_t MASK[4]   = {255, 255, 255, 0};
static const uint8_t GW[4]     = {10, 20, 30, 1};
static const uint8_t IP_B[4]   = {10, 20, 30, 41};
#define PORT 9529

/** @brief 从 IAP 记录里读回 net_cfg 并比对 */
static bool iap_mirror_is(const uint8_t ip[4], uint16_t port)
{
    const app_flash_iap_sys_info_t *r = (const app_flash_iap_sys_info_t *)s_int;
    return memcmp(r->net_cfg.ip, ip, 4) == 0 && r->net_cfg.port == port;
}

/** @brief 从 LDI 记录里读回（重新加载，绕过 g_ldi_ctx 的 RAM 镜像） */
static bool ldi_record_is(const uint8_t ip[4], uint16_t port)
{
    app_flash_ldi_cfg_info_t got;
    memset(&got, 0, sizeof got);
    if (!app_flash_ldi_load_config(&got)) return false;
    return memcmp(got.device_ip, ip, 4) == 0 && got.device_port == port;
}

static bool run_case(void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        g_pass = g_fail = 0;
        fn();
        printf("    → 通过 %d，失败 %d\n", g_pass, g_fail);
        fflush(stdout);
        _exit(g_fail == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ================================================================
 *  分板：本板有没有 IAP 记录区
 *
 *  下面四个用例都建立在"0AH 会把新值同时写进两条记录"之上。直烧板
 *  （BOARD_HAS_IAP_RECORD 0）的 0x08004000 落在固件映像内部，app_iap_cfg.c 的
 *  擦/写原语被整体短路 —— 镜像那条根本不存在，四个用例全部不成立。
 *
 *  本板要守的是短路**在 0AH 这条路径上**也成立：0AH 是写这条记录的两个入口之一
 *  （另一个是 IAP 任务的启动对账），少了短路就是"发一条 0AH 把固件擦了"。
 * ================================================================ */

#if !BOARD_HAS_IAP_RECORD

static void case_0ah_leaves_internal_flash_alone(void)
{
    TEST_BEGIN("直烧板：0AH 只写 LDI 记录，内部 Flash 一个字节都不碰");

    env_setup();

    /* 记录区先塞一份有效记录，让"若短路失效就会真的去擦写"成为可达路径 */
    app_flash_iap_sys_info_t good;
    memset(&good, 0, sizeof good);
    good.magic      = APP_FLASH_IAP_MAGIC;
    good.update_status = APP_FLASH_IAP_UPDATED;
    good.config_crc = _iap_cfg_crc(&good);
    memcpy((void *)g_iap_sys_info, &good, sizeof good);
    pl_flash_stub_reset();

    cmd_set_ip_t req;
    make_0ah(&req, IP_A, MASK, GW, PORT);
    _ldi_cmd_set_ip(NULL, &req);

    CHECK_MSG(pl_flash_stub_erase_count() == 0, "0AH 擦了内部 Flash %d 次 —— 会擦掉固件自身",
              pl_flash_stub_erase_count());
    CHECK_MSG(memcmp((void *)g_iap_sys_info, &good, sizeof good) == 0, "0AH 改动了内部 Flash 内容");

    /* LDI 自己那条记录照常写 —— 短路不该把 0AH 整条路径带停 */
    CHECK_MSG(ldi_record_is(IP_A, PORT), "LDI 记录没写进去，短路把 0AH 也挡住了");
    CHECK_MSG(rsp_status() == 0x00, "回执 status 应为 0x00，实际 0x%02X", rsp_status());

    /* 运行态 IP 不变（"下次上电生效"语义），与有记录区的板一致 */
    CHECK_MSG(s_set_ip_calls == 0, "0AH 改了运行态 IP（应为下次上电生效）");
}

int main(void)
{
    printf("\n\033[33m⚠ 本板 BOARD_HAS_IAP_RECORD = 0（直烧板），"
           "跨记录用例不适用，只跑 0AH 短路守卫\033[0m\n\n");
    fflush(stdout); /* run_case 会 fork —— 不先刷出去，子进程会把这段横幅再打一遍 */

    int failed = 0;
    if (!run_case(case_0ah_leaves_internal_flash_alone)) failed++;

    printf("\n用例 1 个，失败 %d 个\n", failed);
    return failed ? 1 : 0;
}

#else /* BOARD_HAS_IAP_RECORD */

/* ================================================================
 *  用例
 * ================================================================ */

static void case_both_records_written(void)
{
    TEST_BEGIN("0AH 后两条记录都含新值");
    env_setup();

    cmd_set_ip_t req;
    make_0ah(&req, IP_A, MASK, GW, PORT);
    _ldi_cmd_set_ip(NULL, &req);

    CHECK_MSG(iap_mirror_is(IP_A, PORT), "IAP 记录里的 net_cfg 镜像没跟上 0AH");
    CHECK_MSG(ldi_record_is(IP_A, PORT), "LDI 记录没写进去");
    CHECK_MSG(rsp_status() == 0x00, "两条都成功，回执 status 应为 0x00，实际 0x%02X", rsp_status());
    CHECK_MSG(s_tx_count == 1, "0AH 应当只回一帧，实际 %d 帧", s_tx_count);

    /* "下次上电生效"语义：0AH 不得改运行态 IP */
    CHECK_MSG(s_set_ip_calls == 0, "0AH 调了 pl_net_set_ip %d 次 —— 与'下次上电生效'的语义不符",
              s_set_ip_calls);
}

static void case_ldi_failure_does_not_block_mirror(void)
{
    TEST_BEGIN("LDI 记录写失败 → 回执报失败，但 IAP 镜像仍被更新");
    env_setup();

    /* 让 W25Qxx 侧写失败。
       不能改容量来制造失败 —— app_cfg_sched 绑定后会缓存结论（s_storage_bound），
       改 capacity 对它无效（第一版就是这么写错的，白跑一遍才发现）。 */
    s_w25_fail_write = true;

    cmd_set_ip_t req;
    make_0ah(&req, IP_B, MASK, GW, PORT);
    _ldi_cmd_set_ip(NULL, &req);

    CHECK_MSG(rsp_status() == 0x01, "LDI 那条没写成，回执 status 应为 0x01，实际 0x%02X",
              rsp_status());
    /* 这条是核心：镜像那一步不被前一步的失败短路 */
    CHECK_MSG(iap_mirror_is(IP_B, PORT), "LDI 记录写失败把 IAP 镜像也一起挡掉了");

    s_w25_fail_write = false;
}

static void case_mirror_failure_is_invisible_to_host(void)
{
    TEST_BEGIN("IAP 镜像写失败 → 回执仍报成功（已知取舍，钉住它）");
    env_setup();

    /* 让内部 Flash 的编程全部失败（擦除照常） */
    pl_flash_stub_fail_after(0);

    cmd_set_ip_t req;
    make_0ah(&req, IP_B, MASK, GW, PORT);
    _ldi_cmd_set_ip(NULL, &req);

    CHECK_MSG(rsp_status() == 0x00, "LDI 那条是成功的，回执应为 0x00，实际 0x%02X", rsp_status());
    CHECK_MSG(!iap_mirror_is(IP_B, PORT), "本用例的前提是镜像确实没写成 —— 它居然写成了？");

    /* 本用例断言的就是"上位机看不出镜像失败"。这是取舍不是缺陷：
       app_ldi_cmd.c 的注释里写明了回执只反映 LDI 那条，改动时这里会红。 */
    pl_flash_stub_fail_after(-1);
}

static void case_second_0ah_overwrites_both(void)
{
    TEST_BEGIN("连续两次 0AH：两条记录都跟着走");
    env_setup();

    cmd_set_ip_t req;
    make_0ah(&req, IP_A, MASK, GW, PORT);
    _ldi_cmd_set_ip(NULL, &req);
    make_0ah(&req, IP_B, MASK, GW, PORT + 1);
    _ldi_cmd_set_ip(NULL, &req);

    CHECK(iap_mirror_is(IP_B, PORT + 1));
    CHECK(ldi_record_is(IP_B, PORT + 1));
    CHECK(s_tx_count == 2);
}

/* ================================================================ */

/** @brief 在子进程里跑一个用例
 *
 *  fork 的父子各自有独立的 g_pass/g_fail（写时复制），所以**计数必须在子进程里
 *  打印**，父进程只能从退出码知道成败。第一版忘了这点：父进程拿自己的计数器去
 *  比对，既漏统计又重复记失败，输出全是"子进程异常退出"。
 *  fflush 是必须的 —— _exit 不刷 stdio 缓冲，管道下子进程的输出会被整个丢掉。 */

int main(void)
{
    /* 每个用例 fork 独立进程：注册表 / 块位表 / g_ldi_ctx 都是文件级静态且无重置接口，
       与 test_cfg_sched.c 同一处理方式。 */
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"0AH 后两条记录都含新值", case_both_records_written},
        {"LDI 写失败不阻塞 IAP 镜像", case_ldi_failure_does_not_block_mirror},
        {"IAP 镜像失败对上位机不可见", case_mirror_failure_is_invisible_to_host},
        {"连续两次 0AH 两条都跟着走", case_second_0ah_overwrites_both},
    };

    int failed = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        (void)cases[i].name; /* 用例自己用 TEST_BEGIN 打印标题 */
        if (!run_case(cases[i].fn)) failed++;
    }

    printf("\n用例 %zu 个，失败 %d 个\n", sizeof(cases) / sizeof(cases[0]), failed);
    return failed ? 1 : 0;
}

#endif /* BOARD_HAS_IAP_RECORD */
