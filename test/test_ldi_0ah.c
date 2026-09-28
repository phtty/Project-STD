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
#include "app_fold.h"
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

/* ---- app_ldi_ctx_init 的依赖 ----
 * 本用例直接调 app_ldi_ctx_init，--gc-sections 不再能把这段丢掉，故它引用的
 * TCP 客户端/服务端接口也得给全。都是本用例不关心的东西，最小实现即可。 */
void app_tcp_server_set_port(uint16_t port) { (void)port; }
void app_tcp_client_set_remote(const uint8_t ip[4], uint16_t port)
{
    (void)ip;
    (void)port;
}
uint8_t *app_tcp_client_get_host_ip(void)
{
    static uint8_t z[4];
    return z;
}
uint16_t app_tcp_client_get_host_port(void) { return 0; }

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

/* ---- 折叠模块替身（app_ldi_cmd.c 的 EA 分支会调）----
 *
 * app_fold.c 不参与本套件的链接：这里给 3 个可控替身，专门验证 1BH 分派层的**接线**
 * （哪条 Color 走哪个函数、结果如何落 CtlStatus）。app_fold_preset_show/lower_clear
 * 自身的绘制/校验逻辑由 test_fold.c 用生产实现覆盖 —— 两边各测一段，不重复。 */
static app_fold_mode_t s_fold_mode_stub = APP_FOLD_MODE_FLAT;
static int     s_lower_clear_calls;
static int     s_preset_show_calls;
static uint8_t s_preset_show_last_color;
static bool    s_lower_clear_ret = true;
static bool    s_preset_show_ret = true;

app_fold_mode_t app_fold_mode(void) { return s_fold_mode_stub; }
bool app_fold_lower_clear(void)
{
    s_lower_clear_calls++;
    return s_lower_clear_ret;
}
bool app_fold_preset_show(uint8_t color)
{
    s_preset_show_calls++;
    s_preset_show_last_color = color;
    return s_preset_show_ret;
}
void app_udp_broadcast(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}

/* 运行态改 IP 的入口。0AH **不应**调它（"下次上电生效"语义），用例据此断言。
   app_ldi_ctx_init 的 else 分支则**必须**调它（"漏调通知"缺陷的守卫），故同时
   记录最后一次调用的参数，供 else 分支用例比对采纳的地址。 */
static int     s_set_ip_calls;
static uint8_t s_set_ip_last[4];
static uint8_t s_set_ip_mask[4];
static uint8_t s_set_ip_gw[4];
void           pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    memcpy(s_set_ip_last, ip, 4);
    memcpy(s_set_ip_mask, mask, 4);
    memcpy(s_set_ip_gw, gw, 4);
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
    memset(s_set_ip_last, 0, sizeof s_set_ip_last);
    memset(s_set_ip_mask, 0, sizeof s_set_ip_mask);
    memset(s_set_ip_gw, 0, sizeof s_set_ip_gw);
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
 *  1BH EA（雨棚信号灯）显示控制的接线用例
 *
 *  EA 是**折叠变体2 专属**：只有"几何折叠 ∧ 已声明 EA"才有"下半屏 = EA 预置图"。
 *  这些用例只验证 app_ldi_cmd.c 的分派接线与 CtlStatus 落值；app_fold_* 的真实
 *  行为在 test_fold.c。EA 不依赖 IAP 记录，故放在分板 #if 之外，两板都跑。
 * ================================================================ */

/** @brief 声明一块 EA（雨棚信号灯）模块 */
static void declare_ea_module(void)
{
    g_ldi_ctx.cfg.module_count = 2;
    memset(g_ldi_ctx.cfg.modules, 0, sizeof(g_ldi_ctx.cfg.modules));
    g_ldi_ctx.cfg.modules[0].device_type  = APP_LDI_DEVICE_VMS;
    g_ldi_ctx.cfg.modules[0].device_index = 1;
    g_ldi_ctx.cfg.modules[1].device_type  = APP_LDI_DEVICE_CANOPY_LIGHT;
    g_ldi_ctx.cfg.modules[1].device_index = 1;
}

/** @brief 构造一条 1BH 控制请求：单 module（EA）、显示控制 01H、颜色 color */
static uint8_t s_ctrl_buf[128];

static void make_ctrl_ea(uint8_t color)
{
    memset(s_ctrl_buf, 0, sizeof s_ctrl_buf);
    uint8_t *p = s_ctrl_buf + sizeof(app_ldi_ctrl_head_t); /* 头部内容不被读，留零即可 */
    *p++      = 1;                                         /* device_num */
    *p++      = 0;
    *p++      = 4; /* mod_len = type(1)+index(1)+func(1)+color(1) */
    *p++      = (uint8_t)APP_LDI_DEVICE_CANOPY_LIGHT;
    *p++      = 1;    /* device_index */
    *p++      = 0x01; /* device_func_type = 显示控制 */
    *p++      = color;
}

/** @brief 从捕获到的 B1H 响应里取该 module 的 CtlStatus
 *
 *  帧布局：stx(2) ver(1) seq(1) len(4) | ctrl_head(24) | device_num(1) | module[0](4)。
 *  status 是 module 的末字节。 */
static int ctrl_rsp_status(void)
{
    if (s_tx_count == 0) return -1;
    const uint16_t off = (uint16_t)(8 + sizeof(app_ldi_ctrl_head_t) + 1 +
                                    offsetof(ldi_ctrl_rsp_payload_t, status));
    return s_tx_buf[off];
}

/** 变体2：Color=00H → 清下半屏；Color=01H/02H → 取对应预置槽；CtlStatus=00H */
static void case_ctrl_ea_ok_paths(void)
{
    TEST_BEGIN("1BH EA：00H→lower_clear；01H/02H→preset_show，CtlStatus=00H");
    env_setup();
    declare_ea_module();
    s_fold_mode_stub = APP_FOLD_MODE_FOLD_E9_EA;
    s_lower_clear_ret = true;
    s_preset_show_ret = true;

    /* 00H → app_fold_lower_clear，不调 preset_show */
    s_lower_clear_calls = s_preset_show_calls = 0;
    s_tx_count = 0;
    make_ctrl_ea(0x00);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_lower_clear_calls == 1, "00H 应调 app_fold_lower_clear 一次（%d）", s_lower_clear_calls);
    CHECK_MSG(s_preset_show_calls == 0, "00H 不得调 app_fold_preset_show");
    CHECK_MSG(ctrl_rsp_status() == 0x00, "成功时 CtlStatus 应 00H，实际 0x%02X", ctrl_rsp_status());

    /* 02H → app_fold_preset_show(2) */
    s_lower_clear_calls = s_preset_show_calls = 0;
    s_tx_count = 0;
    make_ctrl_ea(0x02);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_preset_show_calls == 1, "02H 应调 app_fold_preset_show 一次（%d）",
              s_preset_show_calls);
    CHECK_MSG(s_preset_show_last_color == 0x02, "preset_show 应收到 Color=02H（得到 0x%02X）",
              s_preset_show_last_color);
    CHECK_MSG(s_lower_clear_calls == 0, "02H 不得调 app_fold_lower_clear");
    CHECK_MSG(ctrl_rsp_status() == 0x00, "成功时 CtlStatus 应 00H，实际 0x%02X", ctrl_rsp_status());
}

/** 变体2：非法颜色 / 预置图拒画 → CtlStatus=01H，且不误调别的函数 */
static void case_ctrl_ea_reject(void)
{
    TEST_BEGIN("1BH EA：非法颜色 / 操作失败 → CtlStatus=01H");
    env_setup();
    declare_ea_module();
    s_fold_mode_stub = APP_FOLD_MODE_FOLD_E9_EA;

    /* 非法颜色（04H）→ 拒绝，两个 fold 函数都不调 */
    s_lower_clear_calls = s_preset_show_calls = 0;
    s_tx_count = 0;
    make_ctrl_ea(0x04);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_lower_clear_calls == 0 && s_preset_show_calls == 0, "非法颜色不得调 fold 控制");
    CHECK_MSG(ctrl_rsp_status() == 0x01, "非法颜色时 CtlStatus 应 01H，实际 0x%02X",
              ctrl_rsp_status());

    /* 操作失败（preset_show 返回 false，如空槽/尺寸不符）→ CtlStatus=01H */
    s_lower_clear_calls = s_preset_show_calls = 0;
    s_preset_show_ret = false;
    s_tx_count = 0;
    make_ctrl_ea(0x01);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_preset_show_calls == 1, "01H 应调 app_fold_preset_show（%d）", s_preset_show_calls);
    CHECK_MSG(ctrl_rsp_status() == 0x01, "操作失败时 CtlStatus 应 01H，实际 0x%02X",
              ctrl_rsp_status());
    s_preset_show_ret = true;
}

/** 未声明 EA：即使分派层进了 switch，也必须再查一次 device_idx 而拒绝 */
static void case_ctrl_ea_unconfigured(void)
{
    TEST_BEGIN("1BH EA：未声明 EA → 不进处理，CtlStatus=01H");
    env_setup();
    /* **显式清空模块表**构造"未声明"场景，不依赖编译期默认表 —— 默认里 EA 开或关
       本用例都应绿（此前写死"默认不声明"的假设，默认一改就红）。 */
    memset(g_ldi_ctx.cfg.modules, 0, sizeof(g_ldi_ctx.cfg.modules));
    g_ldi_ctx.cfg.module_count = 0;
    /* 故意让 mode 替身谎报"变体2"：若实现只信 mode 而不查 device_idx，本用例会红 */
    s_fold_mode_stub = APP_FOLD_MODE_FOLD_E9_EA;

    s_lower_clear_calls = s_preset_show_calls = 0;
    s_tx_count = 0;
    make_ctrl_ea(0x01);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_lower_clear_calls == 0 && s_preset_show_calls == 0,
              "未声明 EA 不得进入处理（lower=%d, preset=%d）", s_lower_clear_calls,
              s_preset_show_calls);
    CHECK_MSG(ctrl_rsp_status() == 0x01, "未声明 EA 时 CtlStatus 应 01H，实际 0x%02X",
              ctrl_rsp_status());
}

/** 非折叠（FLAT）：保持引入折叠前的 TODO 行为 —— 不控制、状态按分派层 */
static void case_ctrl_ea_flat_untouched(void)
{
    TEST_BEGIN("1BH EA：非折叠 → 不控制（保持 TODO），状态按分派层");
    env_setup();
    declare_ea_module(); /* 声明了 EA，但几何非折叠 */
    s_fold_mode_stub = APP_FOLD_MODE_FLAT;

    s_lower_clear_calls = s_preset_show_calls = 0;
    s_tx_count = 0;
    make_ctrl_ea(0x01);
    _ldi_cmd_ctrl(NULL, s_ctrl_buf);
    CHECK_MSG(s_lower_clear_calls == 0 && s_preset_show_calls == 0, "非折叠不得调 fold 控制");
    CHECK_MSG(ctrl_rsp_status() == 0x00,
              "非折叠保持旧行为：声明了模块则状态按 found 给 00H，实际 0x%02X", ctrl_rsp_status());
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

/* ================================================================
 *  app_ldi_ctx_init 的 else 分支：W25Qxx 无配置 → 必须调 pl_net_set_ip
 *
 *  "漏调通知"缺陷的守卫。直烧板上 IAP 记录区不存在（app_iap_get_net_cfg 恒 false），
 *  else 分支用运行态默认值，但**仍必须**把这个决定应用到运行态并触发 IP 变更监听，
 *  否则 LDI 报的 / 实际运行的 / IAP 记录三者漂移。
 *  反向验证：删掉 else 分支里的 pl_net_set_ip，本用例立刻变红（s_set_ip_calls 为 0）。
 * ================================================================ */
static void case_ctx_init_else_notifies_runtime(void)
{
    TEST_BEGIN("直烧板：W25Qxx 无配置 → app_ldi_ctx_init 仍通知运行态");
    env_setup();

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK_MSG(g_ldi_ctx.cfg_valid, "ctx_init 后 cfg_valid 应为 true");
    CHECK_MSG(s_set_ip_calls == 1, "else 分支漏调 pl_net_set_ip（调了 %d 次）", s_set_ip_calls);
}

int main(void)
{
    printf("\n\033[33m⚠ 本板 BOARD_HAS_IAP_RECORD = 0（直烧板），"
           "跨记录用例不适用，只跑 0AH 短路守卫\033[0m\n\n");
    fflush(stdout); /* run_case 会 fork —— 不先刷出去，子进程会把这段横幅再打一遍 */

    int failed = 0;
    if (!run_case(case_0ah_leaves_internal_flash_alone)) failed++;
    if (!run_case(case_ctx_init_else_notifies_runtime)) failed++;

    /* EA 显示控制的接线用例与 IAP 记录无关，两板都跑（见用例区说明） */
    if (!run_case(case_ctrl_ea_ok_paths)) failed++;
    if (!run_case(case_ctrl_ea_reject)) failed++;
    if (!run_case(case_ctrl_ea_unconfigured)) failed++;
    if (!run_case(case_ctrl_ea_flat_untouched)) failed++;

    printf("\n用例 6 个，失败 %d 个\n", failed);
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

/* ================================================================
 *  app_ldi_ctx_init 的 else 分支：W25Qxx 无配置 → 采纳 IAP 记录并通知运行态
 *
 *  "漏调通知"缺陷的守卫。env_setup 后 W25Qxx 是空的（不放 LDI 记录），
 *  load_config 返回 false 走 else 分支；在 IAP 记录区放一份有效记录作为采纳源。
 *  断言：① g_ldi_ctx.cfg 采纳了 IAP 的地址；② pl_net_set_ip 被调用且参数正确。
 *  反向验证：删掉 else 分支里的 pl_net_set_ip，s_set_ip_calls 保持 0，本用例变红。
 * ================================================================ */
static void case_ctx_init_else_adopts_iap_and_notifies(void)
{
    TEST_BEGIN("W25Qxx 无配置：app_ldi_ctx_init 采纳 IAP 记录并通知运行态");
    env_setup();

    /* 只写 IAP 记录，W25Qxx 保持空 → 逼出 else 分支 */
    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);
    CHECK(iap_mirror_is(IP_A, PORT));

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK_MSG(g_ldi_ctx.cfg_valid, "ctx_init 后 cfg_valid 应为 true");
    CHECK_MSG(memcmp(g_ldi_ctx.cfg.device_ip, IP_A, 4) == 0, "else 分支没采纳 IAP 记录里的 IP");
    CHECK_MSG(s_set_ip_calls == 1, "else 分支漏调 pl_net_set_ip（调了 %d 次）—— 运行态与 IAP 镜像不会同步",
              s_set_ip_calls);
    CHECK_MSG(memcmp(s_set_ip_last, IP_A, 4) == 0, "pl_net_set_ip 收到的不是采纳的地址");
}

/* ================================================================
 *  app_ldi_ctx_init 的 cfg_ok 分支：**以 Flash 模块表为准**
 *
 *  用户已定：Flash 有有效配置时，模块表（type/index/vendor/count）整表从记录来；
 *  编译期 g_ldi_ctx 的默认表只是"无有效配置"时的回落。于是同一版固件靠配置即可
 *  切 v1/v2（记录里声明 EA → 折叠变体2）。这些用例在 fork 出的独立进程里跑，
 *  每个用例的 s_load_done 缓存是干净的（save 在前、首次 load 在后）。
 * ================================================================ */

/** @brief 往 W25Qxx 写一份有效 LDI 配置（模块表由调用方给），供 ctx_init 的 cfg_ok 用 */
static void seed_ldi_cfg(const app_flash_ldi_module_cfg_t *mods, uint8_t count)
{
    app_flash_ldi_cfg_info_t cfg = {0};
    memcpy(cfg.device_ip, IP_A, 4);
    cfg.device_port = PORT;
    memcpy(cfg.netmask, MASK, 4);
    memcpy(cfg.gateway, GW, 4);
    cfg.module_count = count;
    for (uint8_t i = 0; i < count && i < APP_FLASH_LDI_MAX_MODULES; i++) cfg.modules[i] = mods[i];

    int32_t st = app_flash_ldi_save_config(&cfg);
    CHECK_MSG(st == 0, "seed：保存配置应成功，返回 %d", (int)st);
}

/** Flash 有效配置 {EA}：整表以记录为准 —— index 从记录来，默认表里的 VMS 被替换掉 */
static void case_ctx_init_modules_from_flash(void)
{
    TEST_BEGIN("Flash 有效配置：模块类型以记录为准（EA 从记录来、默认表被替换）");
    env_setup();

    app_flash_ldi_module_cfg_t mods[1] = {
        {.device_type = APP_LDI_DEVICE_CANOPY_LIGHT, .device_index = 2},
    };
    seed_ldi_cfg(mods, 1);

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK_MSG(g_ldi_ctx.cfg_valid, "cfg_valid 应为 true");
    CHECK_MSG(g_ldi_ctx.cfg.module_count == 1, "module_count 应用记录的 1，得到 %u",
              (unsigned)g_ldi_ctx.cfg.module_count);
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) == 2,
              "EA 应来自记录且 index=2，得到 %u",
              (unsigned)app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT));
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_VMS) == 0xFF,
              "默认表里的 VMS 不该保留（记录里没有它）");
}

/** Flash 有效配置 {E9, EA}：两模块都在（变体2 的声明前提：EA != 0xFF） */
static void case_ctx_init_flash_e9_ea(void)
{
    TEST_BEGIN("Flash 有效配置 {E9,EA}：两模块都在（折叠几何 → 变体2 的声明前提）");
    env_setup();

    app_flash_ldi_module_cfg_t mods[2] = {
        {.device_type = APP_LDI_DEVICE_VMS, .device_index = 1},
        {.device_type = APP_LDI_DEVICE_CANOPY_LIGHT, .device_index = 3},
    };
    seed_ldi_cfg(mods, 2);

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK(g_ldi_ctx.cfg.module_count == 2);
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_VMS) == 1, "E9 的 index 应来自记录");
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) == 3,
              "EA 应声明且 index 来自记录（折叠几何下 app_fold_mode() 即为 v2；真实判定见 test_fold.c）");
}

/** Flash 有效配置仅 {E9}：EA 未声明 */
static void case_ctx_init_flash_e9_only(void)
{
    TEST_BEGIN("Flash 有效配置仅 {E9}：EA 未声明");
    env_setup();

    app_flash_ldi_module_cfg_t mods[1] = {
        {.device_type = APP_LDI_DEVICE_VMS, .device_index = 1},
    };
    seed_ldi_cfg(mods, 1);

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK(app_ldi_get_device_idx(APP_LDI_DEVICE_VMS) == 1);
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) == 0xFF,
              "记录里没有 EA，就不该被声明");
}

/** 无有效配置：保持编译期默认模块表（默认含 EA —— 用户已定的默认配置） */
static void case_ctx_init_no_cfg_keeps_default(void)
{
    TEST_BEGIN("无有效配置：保持编译期默认模块表（默认含 EA）");
    env_setup(); /* W25Qxx 为空 → cfg_ok=false，走 else 分支 */

    const uint8_t before = app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT);

    app_ldi_ctx_init(&g_ldi_ctx);

    CHECK_MSG(g_ldi_ctx.cfg_valid, "cfg_valid 应为 true");
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) == before,
              "无有效配置时应保持编译期默认表（EA index %u → %u）", (unsigned)before,
              (unsigned)app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT));
    CHECK_MSG(before != 0xFF, "当前编译期默认应声明 EA（用户默认配置调整）");
}

/* ================================================================
 *  0BH 设备参数配置：**从请求重建整张模块表**（RAM 与 Flash 一致）
 * ================================================================ */

static uint8_t s_cfg_buf[128];

/** @brief 构造一条 0BH 请求：device_num=1、只给 EA（index=4、vendor=2 字节） */
static void make_0bh_ea_only(void)
{
    memset(s_cfg_buf, 0, sizeof s_cfg_buf);
    app_ldi_req_head_t *head = (app_ldi_req_head_t *)s_cfg_buf;
    head->lane_code[0]        = 0x12; /* 顺带验证 head 被持久化 */
    head->cert_info[0]        = 0x34;

    uint8_t *p = s_cfg_buf + sizeof(app_ldi_req_head_t);
    *p++       = 1;                                     /* device_num */
    *p++       = (uint8_t)APP_LDI_DEVICE_CANOPY_LIGHT;  /* device_type */
    *p++       = 4;                                     /* device_index */
    /* 信号类 module 长度 = head(2) + app_ldi_cfg_signal_t(2)，vendor 剩 2 字节 */
    *p++ = 0xAA;
    *p++ = 0xBB;
}

/** 0BH 用请求的表替换 RAM + 落盘：type/index/vendor/count 全部一致 */
static void case_set_config_rebuilds_table(void)
{
    TEST_BEGIN("0BH：从请求重建整张表（type/index/count），RAM 与 Flash 一致");
    env_setup();

    make_0bh_ea_only();
    _ldi_cmd_set_config(NULL, s_cfg_buf);

    CHECK_MSG(rsp_status() == 0x00, "0BH 应成功，status=0x%02X", rsp_status());
    CHECK_MSG(g_ldi_ctx.cfg.module_count == 1, "module_count 应为请求的 1，得到 %u",
              (unsigned)g_ldi_ctx.cfg.module_count);
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) == 4,
              "EA index 应来自请求（4），得到 %u",
              (unsigned)app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT));
    CHECK_MSG(app_ldi_get_device_idx(APP_LDI_DEVICE_VMS) == 0xFF,
              "请求没给 VMS，就不该留在表里（旧实现只做键匹配、替换不掉默认表）");

    /* 落盘一致：读回记录应与刚提交的 RAM 镜像逐字段一致 */
    app_flash_ldi_cfg_info_t got = {0};
    CHECK_MSG(app_flash_ldi_load_config(&got), "应能读回刚写的记录");
    CHECK_MSG(got.module_count == 1 && got.modules[0].device_type == APP_LDI_DEVICE_CANOPY_LIGHT &&
                  got.modules[0].device_index == 4,
              "Flash 记录的 count/type/index 应与 RAM 一致");
    CHECK_MSG(got.lane_hex[0] == 0x12 && got.cert[0] == 0x34, "0BH 的 lane/cert 也应落盘");
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
        {"W25Qxx 无配置：ctx_init 采纳 IAP 并通知运行态", case_ctx_init_else_adopts_iap_and_notifies},
        {"Flash 有效配置：模块类型以记录为准", case_ctx_init_modules_from_flash},
        {"Flash {E9,EA}：两模块都在", case_ctx_init_flash_e9_ea},
        {"Flash 仅 {E9}：EA 未声明", case_ctx_init_flash_e9_only},
        {"无有效配置：保持编译期默认表", case_ctx_init_no_cfg_keeps_default},
        {"0BH：从请求重建整张表，RAM 与 Flash 一致", case_set_config_rebuilds_table},
        {"EA 显示控制：00H/01H/02H 接线与 CtlStatus", case_ctrl_ea_ok_paths},
        {"EA 显示控制：拒绝路径 → CtlStatus=01H", case_ctrl_ea_reject},
        {"EA 显示控制：未声明 EA 不进处理", case_ctrl_ea_unconfigured},
        {"EA 显示控制：非折叠不动作（保持 TODO）", case_ctrl_ea_flat_untouched},
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
