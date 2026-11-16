/**
 * @file    test_cq_p10.c
 * @brief   CQ_P10（重庆 P10 状态屏固化协议，JSON over UDP）host 单测：探针切帧
 *          （花括号平衡 / 字符串与转义 / 长度上限 / 不吞下一帧）、极简 JSON 解析器
 *          （键精确匹配 / 字符串 / 整数 / IPv4 / 320 元段值）、picset（分段偏移、
 *          8 段齐落盘与指纹去重、写失败重试、nu=3 整屏点亮）、setip（原帧回显 +
 *          网络配置落盘）、固化记录装载与损坏回退、上电网络应用、干接点显示
 *
 * **为什么需要这个测试**：本协议是本产品（2300001735）**唯一**的业务协议，且是
 * "手写 JSON 解析 + 自己管 W25Qxx 尾部固化区"的组合，这两块都不受其它套件覆盖：
 *   · 探针是帧同步的唯一入口。把字符串里的 `}` 数错一格，后面的真帧会被连坐吞掉；
 *     该 WAIT 时判 FAKE 会 skip 1 字节，重扫位移后整帧再也拼不回来（现场表现：
 *     "偶发丢命令，重发几次又能收到"）。
 *   · 段偏移算错一格 = 固化图整幅错位（现场"屏上图像裂成两半"），而协议没有回读
 *     校验、8 段齐才落盘 —— 错一个字节会被永久存进 W25Qxx，掉电不恢复。
 *   · 落盘指纹（s_flushed_crc）是唯一挡住"每个包、每次上电都擦写一遍"的东西：
 *     丢了它，同一幅图重发 8 包就是 8 次 8KB 擦写，W25Qxx 寿命按天算。
 *
 * 手法：TU-include 生产 TU（解析器 / 探针 / 命令层 / 持久化全是 static，外部够不着），
 * 外部依赖换成记录桩：通道发送（观察应答字节与目的端口）、显示原语（观察出图窗口与
 * 位图指针）、W25Qxx（RAM 假 Flash）、CRC（test/stubs/pl_crc_stub.c，与硬件 CRC 单元
 * 同语义）、OS（os_stub 的 pthread 版互斥量）。**真跑**的部分：JSON 解析、探针、
 * picset/setip 全流程、记录布局与 CRC、指纹去重。
 *
 * **面板几何是板级事实，host 测不到**：协议的位图常量按 320×64（P10 模组）定，
 * 套件用同尺寸的假显示实例（320×64）。本分支 board.mk 当前编的仍是 P20 16×8
 * （128×32），与协议的 320×64 不一致 —— 这条差异登记在 docs/测试约定.md §10 的 O8；
 * 换面板前协议侧不会跟着变，现场表现就是花屏/只出一角。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_cq_p10.h"
#include "app_dispatch.h"
#include "app_udp.h"
#include "dev_display.h"
#include "dev_key.h"
#include "dev_storage.h"
#include "dev_w25qxx.h"
#include "pl_crc.h"
#include "pl_net.h"
#include "pl_task.h"
#include "ring_buffer.h"

/* ================================================================
 *  桩：通道发送（观察应答）
 *
 *  cq_reply 用 app_ccb_send_to(ccb, &dst, ...) 而不是 app_ccb_send ——
 *  协议规定上位机收发异端口，应答必须发到**本机监听端口**，不能回本帧来源端口。
 *  所以这里连 dst->port 一起记下来。
 * ================================================================ */

static int      s_tx_calls;
static uint16_t s_tx_port;
static uint16_t s_tx_len;
static uint8_t  s_tx_buf[1600];

int32_t app_ccb_send_to(app_ccb_t *ccb, const app_ccb_dst_t *dst, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    s_tx_calls++;
    s_tx_port = dst ? dst->port : 0;
    s_tx_len  = len;
    uint16_t n = (len < sizeof(s_tx_buf)) ? len : (uint16_t)sizeof(s_tx_buf);
    if (data != nullptr && n > 0) memcpy(s_tx_buf, data, n);
    return 0;
}

static void tx_reset(void)
{
    s_tx_calls = 0;
    s_tx_port  = 0;
    s_tx_len   = 0;
    memset(s_tx_buf, 0, sizeof(s_tx_buf));
}

/* ================================================================
 *  桩：通道绑定 / 网络 / 任务创建 / 按键（模块 init 会摸到，但本套件不跑任务）
 * ================================================================ */

static int s_bind_calls;

void app_dispatch_bind(app_pcb_t *pcb, app_ccb_t *ccb)
{
    (void)pcb;
    (void)ccb;
    s_bind_calls++;
}

app_ccb_t *app_udp_ccb(void)
{
    return nullptr; /* 本套件不跑 init，返回空即可 */
}

static int      s_udp_port_calls;
static uint16_t s_udp_port;

void app_udp_set_port(uint16_t port)
{
    s_udp_port_calls++;
    s_udp_port = port;
}

/* "运行态"网络配置：pl_net_get_ip 从这里返回，pl_net_set_ip 写这里（模拟真的生效） */
static uint8_t s_cur_ip[4]   = {192, 168, 1, 100};
static uint8_t s_cur_mask[4] = {255, 255, 255, 0};
static uint8_t s_cur_gw[4]   = {192, 168, 1, 1};

static int     s_net_set_calls;
static uint8_t s_net_ip[4];
static uint8_t s_net_mask[4];
static uint8_t s_net_gw[4];

void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    memcpy(ip, s_cur_ip, 4);
    memcpy(mask, s_cur_mask, 4);
    memcpy(gw, s_cur_gw, 4);
}

void pl_net_set_ip(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    s_net_set_calls++;
    memcpy(s_net_ip, ip, 4);
    memcpy(s_net_mask, mask, 4);
    memcpy(s_net_gw, gw, 4);
    /* 真实现里改完就生效，后续 get_ip 读到的应是新值 */
    memcpy(s_cur_ip, ip, 4);
    memcpy(s_cur_mask, mask, 4);
    memcpy(s_cur_gw, gw, 4);
}

static void net_stub_reset(void)
{
    s_udp_port_calls = 0;
    s_udp_port       = 0;
    s_net_set_calls  = 0;
    memset(s_net_ip, 0, sizeof(s_net_ip));
    memset(s_net_mask, 0, sizeof(s_net_mask));
    memset(s_net_gw, 0, sizeof(s_net_gw));
    memcpy(s_cur_ip, (const uint8_t[]){192, 168, 1, 100}, 4);
    memcpy(s_cur_mask, (const uint8_t[]){255, 255, 255, 0}, 4);
    memcpy(s_cur_gw, (const uint8_t[]){192, 168, 1, 1}, 4);
}

/** @brief 桩：不真建任务 —— 套件直接调命令层，不跑任务体
 *
 *  cq_handle_task / cq_key_task 都是"取一条 → 处理 → 再取"的无限循环，起线程只会
 *  给断言引入时序不确定性（命令层本身已由 case_handle_frame_dispatch 直接覆盖）。 */
osThreadId_t pl_task_new(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    return nullptr;
}

/** @brief 桩：本套件不验证按键 —— 返回 NULL 让 dev_key_wait_press 恒 false */
dev_key_t *dev_key_get(dev_key_id_t id)
{
    (void)id;
    return nullptr;
}

/* ================================================================
 *  桩：显示原语（观察出图窗口/位图指针，不真画像素）
 *
 *  几何用**产品几何** 320×64：CQ_BITMAP_SIZE(2560) = 320/8 × 64 就是按它定的。
 *  注意本工程 screen_rows 是"宽"、screen_cols 是"高"（见 dev_display.h）。
 * ================================================================ */

#define TEST_PANEL_ROWS (320U) /* 宽 */
#define TEST_PANEL_COLS (64U)  /* 高 */

static dev_display_t s_dsp = {
    .screen_rows = TEST_PANEL_ROWS,
    .screen_cols = TEST_PANEL_COLS,
};

static int      s_fill_calls;
static uint16_t s_fill_w;
static uint16_t s_fill_h;
static uint8_t  s_fill_color;

static int            s_draw_calls;
static const uint8_t *s_draw_ptr;
static uint16_t       s_draw_w;
static uint16_t       s_draw_h;
static uint8_t        s_draw_color;

dev_display_t *dev_display_get(void)
{
    return &s_dsp;
}

void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      dev_display_color_t color)
{
    (void)dev;
    (void)x;
    (void)y;
    s_fill_calls++;
    s_fill_w     = w;
    s_fill_h     = h;
    s_fill_color = (uint8_t)color;
}

void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, dev_display_color_t color)
{
    (void)dev;
    (void)x;
    (void)y;
    s_draw_calls++;
    s_draw_ptr   = bitmap;
    s_draw_w     = w;
    s_draw_h     = h;
    s_draw_color = (uint8_t)color;
}

static void display_reset(void)
{
    s_fill_calls = 0;
    s_fill_w     = 0;
    s_fill_h     = 0;
    s_fill_color = 0xFF;
    s_draw_calls = 0;
    s_draw_ptr   = nullptr;
    s_draw_w     = 0;
    s_draw_h     = 0;
    s_draw_color = 0xFF;
}

/* ================================================================
 *  桩：W25Qxx（RAM 假 Flash）
 *
 *  只按容量/地址语义实现，**不模拟 NOR 的擦除与位与** —— RMW 与扇区擦除是
 *  dev_w25qxx 驱动的职责（真实现在驱动侧），本套件测的是 CQ 模块的记录布局、
 *  CRC 与指纹去重。容量取 64KB：固化记录起点 = capacity - 16384 = 49152，
 *  记录 7716B 落在 [49152, 56868) 内，不越界。
 * ================================================================ */

#define FAKE_CAP (64U * 1024U)

static uint8_t      s_flash[FAKE_CAP];
static dev_storage_t s_dev;
static uint32_t     s_reads;
static uint32_t     s_writes;
static bool         s_write_fail;

static int32_t fake_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)d;
    s_reads++;
    if ((uint64_t)addr + len > FAKE_CAP) return -1;
    memcpy(buf, s_flash + addr, len);
    return 0;
}

static int32_t fake_write(dev_storage_t *d, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)d;
    s_writes++;
    if (s_write_fail) return -1; /* 模拟写失败：CQ 应保留旧指纹并下次重试 */
    if ((uint64_t)addr + len > FAKE_CAP) return -1;
    memcpy(s_flash + addr, buf, len);
    return 0;
}

static uint32_t fake_capacity(dev_storage_t *d)
{
    (void)d;
    return FAKE_CAP;
}

static const dev_storage_ops_t fake_ops = {
    .read     = fake_read,
    .write    = fake_write,
    .capacity = fake_capacity,
};

dev_storage_t *dev_w25qxx_get(void)
{
    s_dev.ops = &fake_ops;
    return &s_dev;
}

/* ================================================================
 *  被测：生产源码本体（TU-include，static 内部结构直接可见）
 *
 *  与 test_cdstd 同一手法：探针、任务、模块资源与命令处理都是 static，外部够不着。
 *  只 GC 掉没人引用的任务体，未引用符号不需要桩。
 * ================================================================ */

#include "../Application/Src/CQP10/app_cq_p10.c"

/* ================================================================
 *  断言与统计（docs/测试约定.md 附录 A 的 A 变体）
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            printf("    \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                 \
            printf("\n");                                                        \
        }                                                                        \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  夹具
 * ================================================================ */

static uint8_t s_scratch[FRAME_DATA_MAX_LEN] __attribute__((aligned(4)));
static char    s_frame[2048];
static app_ccb_t s_msg_ccb = {.name = "cq_ut_chan"};

static uint32_t s_probe_len;
static uint8_t  s_probe_aux;

/** @brief 调一次探针（暂存区容量可调，用于钉"暂存满 → FAKE"） */
static app_pcb_probe_state_t probe(uint16_t scratch_cap)
{
    s_probe_len = 0;
    s_probe_aux = 0xEE; /* 哨兵：本协议不得写 aux */
    return cq_probe_frame(&s_cq_pcb, nullptr, nullptr, s_scratch, scratch_cap, &s_probe_len,
                          &s_probe_aux);
}

/** @brief 清空环形缓冲并放入一段字节 */
static void feed(const void *data, uint16_t len)
{
    rb_flush(&s_cq_rb, nullptr);
    rb_write(&s_cq_rb, (const uint8_t *)data, len, nullptr);
}

/** @brief 把一个 JSON 帧装进 dispatch 消息（模拟框架投递给协议任务的那一格） */
static app_dispatch_msg_t *msg_new(const char *frame, uint16_t len)
{
    static uint8_t buf[CQ_MSG_SIZE] __attribute__((aligned(4)));
    app_dispatch_msg_t *m = (app_dispatch_msg_t *)buf;

    memset(buf, 0, sizeof(buf));
    m->ccb      = &s_msg_ccb;
    m->data_len = len;
    memcpy(m->data, frame, len);
    return m;
}

/** @brief 模块状态复位 = 模拟一次上电：擦除态 Flash → 装载（空记录）→ 清运行态 */
static void fixture_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_reads      = 0;
    s_writes     = 0;
    s_write_fail = false;

    if (s_cq_mutex == nullptr) s_cq_mutex = osMutexNew(nullptr);

    s_cq_rb.read_index  = 0;
    s_cq_rb.write_index = 0;
    s_cq_pcb.rb         = &s_cq_rb;

    cq_persist_load(); /* 全 0xFF → 复位成空记录，并算出 s_persist_addr */

    memset(s_seg, 0, sizeof(s_seg));
    memset(s_flushed_crc, 0, sizeof(s_flushed_crc));
    s_netcfg_valid = false;
    s_cq_port      = CQ_UDP_PORT_DEFAULT;

    tx_reset();
    display_reset();
    net_stub_reset();
}

/** @brief 造 320 字节段值：seed 决定整段内容，便于"改了哪一段"一眼看出 */
static void fill_pattern(uint8_t *v, uint8_t seed)
{
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++)
        v[i] = (uint8_t)(seed + (uint8_t)(i * 7U));
}

/* ---- JSON 帧拼接助手（都返回累计长度；缓冲够大，不做溢出裁剪） ---- */

static size_t app(char *out, size_t n, const char *s)
{
    size_t l = strlen(s);
    memcpy(out + n, s, l);
    return n + l;
}

/** @brief 追加 `"tK":[...]`（数组形式 —— 上位机实际下发的形式） */
static size_t app_t_array(char *out, size_t n, unsigned k, const uint8_t *v, size_t cnt)
{
    n += (size_t)snprintf(out + n, 512, "\"t%u\":[", k);
    for (size_t i = 0; i < cnt; i++) {
        char num[8];
        int  l = snprintf(num, sizeof(num), i ? ",%u" : "%u", (unsigned)v[i]);
        memcpy(out + n, num, (size_t)l);
        n += (size_t)l;
    }
    return app(out, n, "]");
}

/** @brief 追加 `"tK":"..."`（协议文档的形式 —— 逗号分隔的数字字符串） */
static size_t app_t_str(char *out, size_t n, unsigned k, const uint8_t *v, size_t cnt)
{
    n += (size_t)snprintf(out + n, 512, "\"t%u\":\"", k);
    for (size_t i = 0; i < cnt; i++) {
        char num[8];
        int  l = snprintf(num, sizeof(num), i ? ",%u" : "%u", (unsigned)v[i]);
        memcpy(out + n, num, (size_t)l);
        n += (size_t)l;
    }
    return app(out, n, "\"");
}

/** @brief 造一条 picset 帧；seg=0 表示不带任何 t 键 */
static size_t frame_picset(char *out, int nu, char c, unsigned seg, const uint8_t *vals, size_t cnt)
{
    size_t n = app(out, 0, "{\"cmd\":\"picset\",\"nu\":");
    n += (size_t)snprintf(out + n, 64, "%d,\"c\":\"%c\"", nu, c);
    if (seg > 0) {
        n = app(out, n, ",");
        n = app_t_array(out, n, seg, vals, cnt);
    }
    return app(out, n, "}");
}

/* ================================================================
 *  探针：帧同步
 * ================================================================ */

/** @brief 四态各就各位：不足 → WAIT、前导垃圾 → FAKE、完整 → READY（带精确帧长）
 *
 *  探针的返回值直接决定框架动作：WAIT 等数据、FAKE 跳 1 字节重扫、SKIP 整帧丢弃。
 *  total_len 给错一位，dispatch 就会把下一帧的头当成本帧尾巴切掉 —— 症状是
 *  "隔一帧丢一帧"。本用例同时钉住"aux 不写"（本协议不做分类，框架的 aux 保持原值）。 */
static void case_probe_states(void)
{
    TEST_BEGIN("探针四态：WAIT / FAKE / READY（精确帧长）与 aux 不被写");

    fixture_reset();

    /* 最小帧长 12 之前只等，不判死 */
    feed("{\"cmd\":\"}", 9);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_WAIT);

    /* 首字节不是 '{' → 伪帧（框架跳 1 字节重扫） */
    feed("X{\"cmd\":\"picset\"}", 18);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE);

    const char *js = "{\"cmd\":\"picset\",\"nu\":3,\"c\":\"g\"}";
    uint16_t    n  = (uint16_t)strlen(js);
    feed(js, n);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == n, "帧长应为 %u，得到 %u", (unsigned)n, (unsigned)s_probe_len);
    CHECK_MSG(s_probe_aux == 0xEE, "本协议不得写 aux（得到 %02X）", s_probe_aux);

    /* 两帧相连：探针只报第一帧的长度，不得把第二帧吞进来 */
    feed(js, n);
    rb_write(&s_cq_rb, (const uint8_t *)js, n, nullptr);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == n, "连帧时帧长仍应为 %u，得到 %u", (unsigned)n, (unsigned)s_probe_len);
    CHECK_MSG(rb_avail(&s_cq_rb, nullptr) == (uint16_t)(2U * n), "探针只窥视、不消费");
}

/** @brief 花括号必须按 JSON 语义平衡：字符串里的 `}` 与 `\"` 都不算结构
 *
 *  这也是一条"能吞帧"的边界：把字符串内的 `}` 当成帧尾，帧长会变短，dispatch 从
 *  中间切开，后面整条流错位；反过来把转义引号当成字符串结束，则永远等不到收尾
 *  （缓冲只进不出，直到 2816 字节塞满后被判 FAKE）。
 *  本产品的 t 数据是纯数字串，不含花括号，但 `cmd` 等字段未来加长/加上位机注释时
 *  就会踩到这条路径 —— 语言层的错不因"今天不用"而免测。 */
static void case_probe_brace_balance(void)
{
    TEST_BEGIN("探针：字符串内 } 与 \\\" 不参与计数、嵌套对象按最外层收尾");

    fixture_reset();

    /* 字符串里的 } 不算帧尾 */
    const char *a = "{\"cmd\":\"a}b\"}";
    feed(a, (uint16_t)strlen(a));
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == strlen(a), "字符串内的 } 不得提前收尾（得到 %u）",
              (unsigned)s_probe_len);

    /* 转义引号：\" 不结束字符串，其后的 } 仍在串内 */
    const char *b = "{\"a\":\"x\\\"}y\"}";
    feed(b, (uint16_t)strlen(b));
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == strlen(b), "转义引号后的 } 不得收尾（得到 %u）",
              (unsigned)s_probe_len);

    /* 嵌套对象：内层 } 只是深度回落，最外层才收尾 */
    const char *c = "{\"a\":{\"b\":1}}";
    feed(c, (uint16_t)strlen(c));
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == strlen(c), "嵌套帧应按最外层收尾（得到 %u）", (unsigned)s_probe_len);

    /* 未闭合 + 暂存还有空间 → 只等（不能判死） */
    feed("{\"cmd\":\"picset\"", 16);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_WAIT);
}

/** @brief 长度门禁：最小 12 字节、上限 CQ_FRAME_MAX，超上限整帧 SKIP
 *
 *  SKIP 的契约是"给出真实帧长让框架整帧跳过"，不是 FAKE 的 +1 重扫 —— 帧结构本身
 *  合法，只是本协议处理不了；判 FAKE 会把 1400 多字节逐字节重扫一遍（O(n²)）。
 *  另钉住环形缓冲的容量约定：必须装得下两整帧（帧尾到齐前可能已收到下一帧的头）。 */
static void case_probe_limits(void)
{
    TEST_BEGIN("探针：12 字节下限、CQ_FRAME_MAX 上限、超长 SKIP、缓冲容两帧");

    fixture_reset();

    /* 恰好 12 字节的合法最小帧 */
    const char *min = "{\"cmd\":\"ab\"}";
    CHECK_MSG(strlen(min) == 12, "最小合法帧应为 12 字节（%u）", (unsigned)strlen(min));
    feed(min, 12);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK(s_probe_len == 12);

    /* 结构完整但帧长 < 12 → FAKE */
    feed("{}}xxxxxxxxxxxx", 15);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_FAKE);

    /* 恰好到上限 → READY；多 1 字节 → SKIP 且帧长精确 */
    for (uint32_t total = CQ_FRAME_MAX; total <= CQ_FRAME_MAX + 1U; total++) {
        size_t n = app(s_frame, 0, "{\"a\":\"");
        while (n < total - 2) s_frame[n++] = 'x'; /* 收尾还有 \"} 两字节 */
        n = app(s_frame, n, "\"}");
        CHECK_MSG(n == total, "构造帧长应为 %u（得到 %u）", (unsigned)total, (unsigned)n);

        feed(s_frame, (uint16_t)n);
        if (total == CQ_FRAME_MAX) {
            CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
            CHECK(s_probe_len == total);
        } else {
            CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_SKIP);
            CHECK_MSG(s_probe_len == total, "SKIP 必须回真实帧长 %u（得到 %u）", (unsigned)total,
                      (unsigned)s_probe_len);
        }
    }

    /* 暂存区塞满仍无收尾 → 超长伪帧；还有空间时只等 */
    for (size_t i = 0; i < 200; i++) s_frame[i] = (i == 0) ? '{' : 'a';
    feed(s_frame, 200);
    CHECK(probe(64) == APP_PCB_PROBE_STATE_FAKE);
    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_WAIT);

    /* 容量约定：至少两整帧 */
    CHECK_MSG(s_cq_rb.size >= 2U * CQ_FRAME_MAX, "环形缓冲 %u 装不下两帧 %u",
              (unsigned)s_cq_rb.size, (unsigned)CQ_FRAME_MAX);
}

/* ================================================================
 *  极简 JSON 解析器
 * ================================================================ */

/** @brief 键必须整词匹配 —— `"c"` 不能命中 `"cmd"` / `"ca"`
 *
 *  颜色字段是单字符键 `"c"`，而每条命令都有 `"cmd"`。若按键名前缀匹配，picset 的
 *  颜色会从 `"picset"` 里取出 'p'，直接判非法而整包丢弃 —— 上线症状是"picset 全不
 *  生效、setip 正常"。冒号后的空白（上位机排版习惯）也要吃掉。 */
static void case_json_key_lookup(void)
{
    TEST_BEGIN("JSON 取值：整词键匹配、冒号后空白、缺键返回空");

    const char *js = "{\"cmd\":\"picset\",\"c\":\"g\",\"ca\":1}";
    uint32_t    avail;
    const char *v;

    v = cq_json_value(js, (uint32_t)strlen(js), "cmd", &avail);
    CHECK(v && avail >= 6 && memcmp(v, "\"picset\"", 8) == 0);

    v = cq_json_value(js, (uint32_t)strlen(js), "c", &avail);
    CHECK_MSG(v && *v == '"' && memcmp(v, "\"g\"", 3) == 0, "c 应取到 \\\"g\\\" 而非 cmd");

    /* 只有 cmd 时，查 "c" 必须查不到（前缀/子串匹配会在这里翻车） */
    const char *only = "{\"cmd\":\"picset\"}";
    CHECK(cq_json_value(only, (uint32_t)strlen(only), "c", &avail) == nullptr);

    /* "ca" 存在，但查 "c" 不得命中它 */
    v = cq_json_value(js, (uint32_t)strlen(js), "ca", &avail);
    CHECK(v && *v == '1');
    CHECK(cq_json_value("{\"ca\":1}", 8, "c", &avail) == nullptr);

    /* 冒号后的空白 */
    const char *ws = "{\"nu\" :  \t 5}";
    v              = cq_json_value(ws, (uint32_t)strlen(ws), "nu", &avail);
    CHECK_MSG(v && *v == '5', "冒号后空白应被跳过");

    /* 缺键 */
    CHECK(cq_json_value(js, (uint32_t)strlen(js), "zz", &avail) == nullptr);
}

/** @brief 字符串值：引号必须闭合、值内不得有转义、超 cap 拒绝
 *
 *  cap 是含 NUL 的容量。差一位就是栈溢出（CQ 里 buf[20] 装 IP）或被静默截断
 *  （截断后的 IP 仍可能"看起来合法"，于是把错的网配写进 Flash）。 */
static void case_json_str_parse(void)
{
    TEST_BEGIN("JSON 字符串：闭合、空串、转义拒绝、cap 边界");

    char     out[16];
    uint32_t n = 0;

    CHECK(cq_json_str("{\"c\":\"g\"}", 9, "c", out, sizeof(out), &n));
    CHECK(n == 1 && out[0] == 'g' && out[1] == '\0');

    /* 空串合法（长度 0） */
    CHECK(cq_json_str("{\"c\":\"\"}", 10, "c", out, sizeof(out), &n));
    CHECK(n == 0 && out[0] == '\0');

    /* 引号未闭合 */
    CHECK(!cq_json_str("{\"c\":\"g}", 9, "c", out, sizeof(out), &n));

    /* 值内含转义 → 协议不接受 */
    CHECK(!cq_json_str("{\"c\":\"a\\\"b\"}", 13, "c", out, sizeof(out), &n));

    /* cap 不够（4 字节值 + NUL 需要 5） */
    CHECK(!cq_json_str("{\"c\":\"abcd\"}", 12, "c", out, 4, &n));
    CHECK(cq_json_str("{\"c\":\"abc\"}", 11, "c", out, 4, &n));

    /* 值不是字符串 */
    CHECK(!cq_json_str("{\"nu\":5}", 9, "nu", out, sizeof(out), &n));
}

/** @brief 整数：范围双夹、溢出防护、值后只接受 , } ] 空白
 *
 *  只判上界会漏掉"先转换再判据"的回绕（负数/巨数变成小正数被放行）；范围由调用方
 *  给，nu 是 0..3、port 是 1..65535 —— 两条边都要真的能挡住。 */
static void case_json_int_parse(void)
{
    TEST_BEGIN("JSON 整数：范围双夹、溢出、值后字符白名单");

    uint32_t v = 0;

    CHECK(cq_json_int("{\"nu\":3}", 9, "nu", 0, 3, &v) && v == 3);
    CHECK(cq_json_int("{\"nu\":0}", 9, "nu", 0, 3, &v) && v == 0);

    /* 上界 / 下界各越 1 */
    CHECK(!cq_json_int("{\"nu\":4}", 9, "nu", 0, 3, &v));
    CHECK(!cq_json_int("{\"port\":65536}", 13, "port", 1, 65535, &v));
    CHECK(!cq_json_int("{\"port\":0}", 11, "port", 1, 65535, &v));
    CHECK(cq_json_int("{\"port\":65535}", 13, "port", 1, 65535, &v) && v == 65535);

    /* 负数 / 非数字开头 */
    CHECK(!cq_json_int("{\"nu\":-1}", 10, "nu", 0, 3, &v));

    /* 值后跟字母 → 不是整数（"3x" 不能当 3） */
    CHECK(!cq_json_int("{\"nu\":3x}", 10, "nu", 0, 3, &v));

    /* 溢出：超出 uint32 的十进制串必须在累加前拦住 */
    CHECK(!cq_json_int("{\"nu\":99999999999}", 18, "nu", 0, 3, &v));

    /* 合法收尾字符：} 、逗号、右方括号、空白 */
    CHECK(cq_json_int("{\"nu\":1}", 9, "nu", 0, 3, &v) && v == 1);
    CHECK(cq_json_int("{\"nu\":1,\"c\":\"g\"}", 17, "nu", 0, 3, &v) && v == 1);
    CHECK(cq_json_int("{\"nu\":1 }", 10, "nu", 0, 3, &v) && v == 1);
    CHECK(cq_json_int("{\"nu\":1]}", 9, "nu", 0, 3, &v) && v == 1);
}

/** @brief 点分十进制：四段、每段 1~3 位、≤255；段数/位数/字符全要挡住
 *
 *  网配写进 Flash 后重启才生效，写错一段的后果是"设备从此不在网上"（要拆机或
 *  用别的通道救），因此在解析层就把非法值拒掉，比事后救砖便宜得多。 */
static void case_parse_ipv4(void)
{
    TEST_BEGIN("IPv4 解析：四段、每段 ≤255、位数与字符限制");

    static const struct {
        const char *s;
        bool        ok;
        uint8_t     ip[4];
    } cases[] = {
        {"192.168.1.10", true, {192, 168, 1, 10}},
        {"0.0.0.0", true, {0, 0, 0, 0}},
        {"255.255.255.255", true, {255, 255, 255, 255}},
        {"256.1.1.1", false, {0, 0, 0, 0}},
        {"1.2.3.1234", false, {0, 0, 0, 0}},
        {"1.2.3", false, {0, 0, 0, 0}},
        {"1.2.3.4.5", false, {0, 0, 0, 0}},
        {"1..2.3", false, {0, 0, 0, 0}},
        {"1.2.3.", false, {0, 0, 0, 0}},
        {"1.2.3.4a", false, {0, 0, 0, 0}},
        {"", false, {0, 0, 0, 0}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t out[4] = {0xAA, 0xAA, 0xAA, 0xAA};
        bool    ok     = cq_parse_ipv4(cases[i].s, (uint32_t)strlen(cases[i].s), out);

        CHECK_MSG(ok == cases[i].ok, "\"%s\" 判据应为 %d", cases[i].s, (int)cases[i].ok);
        if (cases[i].ok)
            CHECK_MSG(memcmp(out, cases[i].ip, 4) == 0, "\"%s\" 解析出的字节不对", cases[i].s);
    }
}

/** @brief 320 元段值：两种线上格式、恰好 320 个、0~255
 *
 *  文档写的是 `"t1":"0,255,..."`（字符串），上位机实际发的是 `"t1":[0,255,...]`
 *  （裸数组）—— 两种都收，且**恰好** 320 个：少一个 → 该段整段作废（屏幕上那一
 *  8 行保持上一次的内容）；多一个 → 越界写风险，必须拒绝。 */
static void case_parse_t_value(void)
{
    TEST_BEGIN("段值解析：字符串/数组两种形式、恰 320 个、值域 0~255");

    uint8_t vals[CQ_PACKET_PAYLOAD];
    uint8_t out[CQ_PACKET_PAYLOAD];
    size_t  n;

    fill_pattern(vals, 3);

    /* 数组形式 */
    n = app(s_frame, 0, "{");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    CHECK(cq_parse_t_value(s_frame, (uint32_t)n, "t1", out));
    CHECK(memcmp(out, vals, CQ_PACKET_PAYLOAD) == 0);

    /* 字符串形式（协议文档口径） */
    n = app(s_frame, 0, "{");
    n = app_t_str(s_frame, n, 2, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    CHECK(cq_parse_t_value(s_frame, (uint32_t)n, "t2", out));
    CHECK(memcmp(out, vals, CQ_PACKET_PAYLOAD) == 0);

    /* 少一个 / 多一个 */
    n = app(s_frame, 0, "{");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD - 1);
    n = app(s_frame, n, "}");
    CHECK(!cq_parse_t_value(s_frame, (uint32_t)n, "t1", out));

    uint8_t more[CQ_PACKET_PAYLOAD + 1]; /* 多一个：必须由数组本身提供，不能越界读 vals */
    memcpy(more, vals, CQ_PACKET_PAYLOAD);
    more[CQ_PACKET_PAYLOAD] = 0x77;
    n = app(s_frame, 0, "{");
    n = app_t_array(s_frame, n, 1, more, CQ_PACKET_PAYLOAD + 1);
    n = app(s_frame, n, "}");
    CHECK(!cq_parse_t_value(s_frame, (uint32_t)n, "t1", out));

    /* 值域：256 越界、空段、尾逗号、非法字符 */
    CHECK(!cq_parse_t_value("{\"t1\":[256]}", 12, "t1", out));
    CHECK(!cq_parse_t_value("{\"t1\":[1,,2]}", 13, "t1", out));
    CHECK(!cq_parse_t_value("{\"t1\":[1,2,]}", 13, "t1", out));
    CHECK(!cq_parse_t_value("{\"t1\":[1,a]}", 12, "t1", out));

    /* 键不存在 */
    CHECK(!cq_parse_t_value("{\"t1\":[1]}", 11, "t2", out));

    /* 数字间空白宽容（值后不留空格：那会让结束符前 digits==0）*/
    n = app(s_frame, 0, "{\"t1\":[");
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++) {
        char num[8];
        int  l = snprintf(num, sizeof(num), i ? ", %u" : "%u", (unsigned)vals[i]);
        memcpy(s_frame + n, num, (size_t)l);
        n += (size_t)l;
    }
    n = app(s_frame, n, "]}");
    CHECK(cq_parse_t_value(s_frame, (uint32_t)n, "t1", out));
    CHECK(memcmp(out, vals, CQ_PACKET_PAYLOAD) == 0);
}

/** @brief 空白宽容：`\r` `\n` 与空格等价（上位机是 Windows、JSON 常被折行）
 *
 *  现场可能的样子：抓包看到一条完全合法的 JSON，本机却一声不吭 —— 因为它被 CRLF
 *  排版过（换行在键之间、或数组每 N 个值折一次行）。协议解析器若只认空格/制表符，
 *  整包会被判非法**静默丢弃**：picset 少一条 = 那 8 行永远黑（"上半屏缺一块"），
 *  setip 少一条 = 改址不回显（现场以为设备死机）。
 *
 *  JSON 规范里 `\r \n \t 空格` 都是空白，设备没理由只收其中两样。注意帧长上限仍按
 *  1408 字节算 —— 折行不能把帧撑爆（每 40 个值折一次是现实排版）。 */
static void case_json_whitespace_crlf(void)
{
    TEST_BEGIN("空白宽容：CRLF / 折行 / 冒号后换行都算合法 JSON");

    char     out[16];
    uint32_t n = 0;

    /* 冒号前后与值两侧都有 CRLF */
    const char *f1 = "{\r\n  \"c\"\r\n :\r\n \"g\"\r\n}";
    CHECK_MSG(cq_json_str(f1, (uint32_t)strlen(f1), "c", out, sizeof(out), &n) && n == 1 && out[0] == 'g',
              "CRLF 排版下取 \"c\" 失败（got len=%u）", n);

    /* 整数后跟 CRLF */
    uint32_t v = 0;
    CHECK_MSG(cq_json_int("{\"nu\": 3\r\n}", 12, "nu", 0, 3, &v) && v == 3, "整数后 CRLF 应合法（got %u）", v);

    /* 段值数组折行：每 40 个值一个 CRLF */
    uint8_t vals[CQ_PACKET_PAYLOAD];
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++) vals[i] = (uint8_t)(i % 200U);

    size_t k = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"g\",\"t1\":[");
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++) {
        char num[16];
        int  l = snprintf(num, sizeof(num), (i == 0) ? "%u" : (i % 40 == 0 ? ",\r\n%u" : ",%u"),
                          (unsigned)vals[i]);
        memcpy(s_frame + k, num, (size_t)l);
        k += (size_t)l;
    }
    k = app(s_frame, k, "]}");
    CHECK_MSG(k <= CQ_FRAME_MAX, "折行后的帧长 %u 超上限", (unsigned)k);

    uint8_t parsed[CQ_PACKET_PAYLOAD];
    CHECK_MSG(cq_parse_t_value(s_frame, (uint32_t)k, "t1", parsed), "折行的 t1 应能解析");
    CHECK(memcmp(parsed, vals, CQ_PACKET_PAYLOAD) == 0);

    /* 整帧贯通：Windows 排版（键之间 CRLF + 每 40 个值折行）→ 必须照收并回 OK */
    fixture_reset();
    size_t m = app(s_frame, 0, "{\r\n  \"cmd\": \"picset\",\r\n  \"nu\": 0,\r\n  \"c\": \"g\",\r\n  \"t2\": [");
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++) {
        char num[16];
        int  l = snprintf(num, sizeof(num), (i == 0) ? "%u" : (i % 40 == 0 ? ",\r\n    %u" : ",%u"),
                          (unsigned)vals[i]);
        memcpy(s_frame + m, num, (size_t)l);
        m += (size_t)l;
    }
    m = app(s_frame, m, "\r\n  ]\r\n}\r\n");
    CHECK_MSG(m <= CQ_FRAME_MAX, "整帧 %u 超上限", (unsigned)m);

    cq_handle_frame(msg_new(s_frame, (uint16_t)m));
    CHECK_MSG(s_tx_calls == 1, "Windows 排版的 picset 应被接收并回 OK（得到 %d 次应答）", s_tx_calls);
    CHECK(s_cq.slot[0].color == DEV_DISPLAY_COLOR_GREEN);
    CHECK(memcmp(&s_cq.slot[0].bitmap[CQ_PACKET_PAYLOAD], vals, CQ_PACKET_PAYLOAD) == 0);
}

/* ================================================================
 *  picset：分段固化
 * ================================================================ */

/** @brief picset 正例：段落在整幅位图的 (K-1)*320 偏移，回 OK 到监听端口
 *
 *  偏移错一位 = 整幅错位（协议无回读校验，错图会被固化进 Flash 且掉电不恢复）。
 *  同时钉住"未满 8 段不落盘"：单包就写 Flash 的话，一次下发要擦写 8 次。
 *  应答目的端口必须是本机监听端口（上位机收发异端口）。 */
static void case_picset_segment(void)
{
    TEST_BEGIN("picset：段偏移 (K-1)*320、回 OK、未满 8 段不落盘");

    fixture_reset();

    uint8_t vals[CQ_PACKET_PAYLOAD];
    fill_pattern(vals, 0x11);

    size_t n = frame_picset(s_frame, 0, 'g', 3, vals, CQ_PACKET_PAYLOAD);
    CHECK_MSG(n <= CQ_FRAME_MAX, "帧长 %u 超过协议上限", (unsigned)n);

    cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    CHECK_MSG(s_tx_calls == 1, "合法 picset 应回一帧（得到 %d）", s_tx_calls);
    CHECK_MSG(s_tx_port == CQ_UDP_PORT_DEFAULT, "应答应发往监听端口 %u（得到 %u）",
              (unsigned)CQ_UDP_PORT_DEFAULT, (unsigned)s_tx_port);
    CHECK_MSG(s_tx_len == 25 && memcmp(s_tx_buf, "{\"cmd\":\"picset\",\"t\":\"OK\"}", 25) == 0,
              "应答体应固定为 picset/OK");

    CHECK(s_cq.slot[0].color == DEV_DISPLAY_COLOR_GREEN);
    CHECK_MSG(memcmp(&s_cq.slot[0].bitmap[2U * CQ_PACKET_PAYLOAD], vals, CQ_PACKET_PAYLOAD) == 0,
              "t3 应落在偏移 %u", (unsigned)(2U * CQ_PACKET_PAYLOAD));

    /* 邻接区未被写：偏移算成 K*320 或 (K-1)*320-1 都会在这里红 */
    CHECK(s_cq.slot[0].bitmap[2U * CQ_PACKET_PAYLOAD - 1U] == 0);
    CHECK(s_cq.slot[0].bitmap[3U * CQ_PACKET_PAYLOAD] == 0);
    CHECK(s_seg[0] == (uint8_t)(1U << 2));
    CHECK_MSG(s_writes == 0, "未满 8 段不得落盘（得到 %u 次写）", (unsigned)s_writes);

    /* 另一槽另一段：互不串槽 */
    uint8_t vals2[CQ_PACKET_PAYLOAD];
    fill_pattern(vals2, 0x80);
    n = frame_picset(s_frame, 2, 'y', 7, vals2, CQ_PACKET_PAYLOAD);
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    CHECK(s_cq.slot[2].color == DEV_DISPLAY_COLOR_YELLOW);
    CHECK(memcmp(&s_cq.slot[2].bitmap[6U * CQ_PACKET_PAYLOAD], vals2, CQ_PACKET_PAYLOAD) == 0);
    CHECK(s_cq.slot[0].color == DEV_DISPLAY_COLOR_GREEN); /* 槽 0 不受影响 */
    CHECK(s_seg[2] == (uint8_t)(1U << 6));
    CHECK(s_writes == 0);
}

/** @brief picset 反例矩阵：非法包整包丢弃且**零副作用**
 *
 *  每个分支都对应一种现场数据：nu 越界（上位机协议版本不符）、颜色字符错（配置
 *  表里填了字母）、t 缺失/重复（分包乱序或重传）、段值个数或值域错（文本被截断）。
 *  反例若"先写一半再发现不对"，屏幕上就是半幅残图，且 8 段齐时会被固化。 */
static void case_picset_rejects(void)
{
    TEST_BEGIN("picset 反例：nu/c/t 缺失与越界 → 零副作用（不回、不写、不改缓存）");

    fixture_reset();

    /* 反例基数：**一条本来会通过的帧**，每行只坏一处。
     *
     * 这一点决定了反例矩阵有没有价值：若"坏 nu"的帧同时缺 t，被测分支（nu 范围）
     * 之外还有第二道拒绝理由兜着，把 nu 校验删掉这条用例仍然全绿（变异测试实测
     * 存活）。所以每条反例都必须带上合法的 320 元 t —— 只留**一个**拒绝理由。
     *
     * 用个位数取值（i%10）是为了让"两个 t 键"的那条帧仍塞进 1408 字节上限：
     * 两个 320 元数组各约 641 字节。 */
    uint8_t vals[CQ_PACKET_PAYLOAD];
    for (size_t i = 0; i < CQ_PACKET_PAYLOAD; i++) vals[i] = (uint8_t)(i % 10U);

    size_t n;

    /* 1) nu 越界（其余全合法） */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":4,\"c\":\"g\",");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 2) 颜色字符非法（其余全合法）：'b' 若被放行会成为"黑色"，静默吞掉整包 */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"b\",");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 3) 缺 t 键 */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"g\"}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 4) 两个 t 键：必须是**两段都合法**，否则又变成"靠个数不符兜底" */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"g\",");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, ",");
    n = app_t_array(s_frame, n, 2, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    CHECK_MSG(n <= CQ_FRAME_MAX, "两个 t 键的帧应仍在上限内（%u）", (unsigned)n);
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 5) 缺 c 键（其余全合法） */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,");
    n = app_t_array(s_frame, n, 1, vals, CQ_PACKET_PAYLOAD);
    n = app(s_frame, n, "}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 6) 段值个数少 1（合法 320 个之外只少一个） */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"g\",");
    n = app_t_array(s_frame, n, 4, vals, CQ_PACKET_PAYLOAD - 1);
    n = app(s_frame, n, "}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 7) 值越界（256） */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":0,\"c\":\"g\",\"t4\":[256]}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    /* 全部反例之后：一次应答都没有、一次写都没有、缓存原样 */
    CHECK_MSG(s_tx_calls == 0, "非法包不得应答（得到 %d 次）", s_tx_calls);
    CHECK_MSG(s_writes == 0, "非法包不得落盘（得到 %u 次）", (unsigned)s_writes);
    CHECK_MSG(s_seg[0] == 0, "非法包不得推进段位图（得到 %02X）", s_seg[0]);
    CHECK(s_cq.slot[0].color == 0xFF);
    for (size_t i = 0; i < CQ_BITMAP_SIZE; i++) {
        if (s_cq.slot[0].bitmap[i] != 0) {
            CHECK_MSG(false, "非法包改动了位图（偏移 %u）", (unsigned)i);
            break;
        }
    }
}

/** @brief 8 段齐才落盘 + 指纹去重 + 写失败保留旧指纹
 *
 *  W25Qxx 擦写寿命是有限的，这条链路上一共有三处放大器：每包写、每次重发写、
 *  每次上电写。指纹（槽内容 CRC）同时挡掉后两者 —— 本用例把三次放大各钉一遍。
 *  "写失败要保留旧指纹"是重试的唯一依据：若失败也更新指纹，这一幅图就再也写不
 *  进去了（直到内容变化或重启）。 */
static void case_picset_flush_dedupe(void)
{
    TEST_BEGIN("picset 落盘：8 段齐写一次、同内容重发不写、写失败下次重试");

    fixture_reset();

    uint8_t vals[CQ_PACKET_PAYLOAD];

    for (unsigned k = 1; k <= 8; k++) {
        fill_pattern(vals, (uint8_t)k);
        size_t n = frame_picset(s_frame, 1, 'y', k, vals, CQ_PACKET_PAYLOAD);
        cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    }

    CHECK_MSG(s_writes == 1, "8 段齐应只写一次（得到 %u 次）", (unsigned)s_writes);
    CHECK(cq_persist_valid()); /* 整条记录 CRC 自洽 */
    CHECK(s_flushed_crc[1] != 0);
    fill_pattern(vals, 1);
    CHECK(memcmp(&s_cq.slot[1].bitmap[0], vals, CQ_PACKET_PAYLOAD) == 0);
    fill_pattern(vals, 8);
    CHECK(memcmp(&s_cq.slot[1].bitmap[7U * CQ_PACKET_PAYLOAD], vals, CQ_PACKET_PAYLOAD) == 0);

    /* 同一幅图重发 8 段 → 指纹相同，不得再擦写 */
    for (unsigned k = 1; k <= 8; k++) {
        fill_pattern(vals, (uint8_t)k);
        size_t n = frame_picset(s_frame, 1, 'y', k, vals, CQ_PACKET_PAYLOAD);
        cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    }
    CHECK_MSG(s_writes == 1, "内容未变不得重写（得到 %u 次）", (unsigned)s_writes);

    /* 改一段 → 内容变了，必须重写一次 */
    for (unsigned k = 1; k <= 8; k++) {
        fill_pattern(vals, (uint8_t)(k == 4 ? 0x99 : k));
        size_t n = frame_picset(s_frame, 1, 'y', k, vals, CQ_PACKET_PAYLOAD);
        cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    }
    CHECK_MSG(s_writes == 2, "内容变了应重写（得到 %u 次）", (unsigned)s_writes);

    /* 写失败：指纹保持旧值 → 重发同样内容会再试一次并成功 */
    uint32_t fp_before = s_flushed_crc[1];
    s_writes           = 0;
    s_write_fail       = true;
    for (unsigned k = 1; k <= 8; k++) {
        fill_pattern(vals, (uint8_t)(k == 6 ? 0x33 : (k == 4 ? 0x99 : k)));
        size_t n = frame_picset(s_frame, 1, 'y', k, vals, CQ_PACKET_PAYLOAD);
        cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    }
    CHECK_MSG(s_writes == 1, "写失败也要试过（得到 %u 次）", (unsigned)s_writes);
    CHECK_MSG(s_flushed_crc[1] == fp_before, "写失败不得更新指纹");

    s_write_fail = false;
    for (unsigned k = 1; k <= 8; k++) {
        fill_pattern(vals, (uint8_t)(k == 6 ? 0x33 : (k == 4 ? 0x99 : k)));
        size_t n = frame_picset(s_frame, 1, 'y', k, vals, CQ_PACKET_PAYLOAD);
        cq_handle_frame(msg_new(s_frame, (uint16_t)n));
    }
    CHECK_MSG(s_writes == 2, "重试应真的再写一次（得到 %u 次）", (unsigned)s_writes);
    CHECK(s_flushed_crc[1] != fp_before);
}

/** @brief nu=3 整屏点亮：不解析 t、不固化、应答照旧
 *
 *  这是现场"临时全亮"的用法：只发一包就能点屏，屏上内容不落 Flash。若误走固化
 *  分支，好端端的三幅固化图里的一幅会被单色覆盖（掉电不恢复）。 */
static void case_picset_fill_all(void)
{
    TEST_BEGIN("picset nu=3：整屏填充不固化，带畸形 t 也不影响");

    fixture_reset();

    size_t n = frame_picset(s_frame, 3, 'y', 0, nullptr, 0);
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    CHECK_MSG(s_fill_calls == 1, "nu=3 应整屏填充一次（得到 %d）", s_fill_calls);
    CHECK_MSG(s_fill_w == TEST_PANEL_ROWS && s_fill_h == TEST_PANEL_COLS,
              "填充窗口应为整屏 %ux%u（得到 %ux%u）", (unsigned)TEST_PANEL_ROWS,
              (unsigned)TEST_PANEL_COLS, (unsigned)s_fill_w, (unsigned)s_fill_h);
    CHECK(s_fill_color == DEV_DISPLAY_COLOR_YELLOW);
    CHECK_MSG(s_writes == 0, "nu=3 不得固化（得到 %u 次写）", (unsigned)s_writes);
    CHECK(s_tx_calls == 1 && s_tx_len == 25);

    /* nu=3 不解析 t：带一段畸形 t1 仍应点亮（上面的 8 段解析不参与本分支） */
    n = app(s_frame, 0, "{\"cmd\":\"picset\",\"nu\":3,\"c\":\"r\",\"t1\":\"boom\"}");
    cq_handle_frame(msg_new(s_frame, (uint16_t)n));

    CHECK_MSG(s_fill_calls == 2, "nu=3 不应被 t 数据拦住");
    CHECK(s_fill_color == DEV_DISPLAY_COLOR_RED);
    CHECK(s_writes == 0);
}

/* ================================================================
 *  setip：修改 IP
 * ================================================================ */

/** @brief setip 正例：ip/mask/wg/port 全合法才写记录，原帧逐字节回显
 *
 *  这是"现场唯一能改网配的入口"（本产品无 IAP、无 RS232 配置通道）：写错就该整包
 *  丢弃，绝不能只写一半（比如 ip 合法但 port 非法时把 ip 先存了，重启后设备换了
 *  IP 却还听旧端口，现场直接失联）。 */
static void case_setip(void)
{
    TEST_BEGIN("setip：四字段全合法才落盘，原帧回显，记录 CRC 自洽");

    fixture_reset();

    const char *js = "{\"cmd\":\"setip\",\"ip\":\"192.168.1.77\",\"mask\":\"255.255.255.0\","
                     "\"wg\":\"192.168.1.1\",\"port\":20103}";
    uint16_t    n  = (uint16_t)strlen(js);

    cq_handle_frame(msg_new(js, n));

    CHECK_MSG(s_tx_calls == 1, "合法 setip 应回一帧（得到 %d）", s_tx_calls);
    CHECK_MSG(s_tx_len == n && memcmp(s_tx_buf, js, n) == 0, "setip 应原帧逐字节回显");

    CHECK_MSG(s_cq.netcfg.magic == CQ_NETCFG_MAGIC, "记录魔数应为 CQN1");
    CHECK(memcmp(s_cq.netcfg.ip, (const uint8_t[]){192, 168, 1, 77}, 4) == 0);
    CHECK(memcmp(s_cq.netcfg.mask, (const uint8_t[]){255, 255, 255, 0}, 4) == 0);
    CHECK(memcmp(s_cq.netcfg.gw, (const uint8_t[]){192, 168, 1, 1}, 4) == 0);
    CHECK(s_cq.netcfg.port == 20103);
    CHECK_MSG(s_cq.netcfg.crc32 ==
                  pl_crc32_calc(pl_crc_get_handle(), (const uint8_t *)&s_cq.netcfg, 20),
              "内层 CRC 应覆盖前 20 字节");
    CHECK(s_netcfg_valid);
    CHECK_MSG(s_writes == 1, "应落盘一次（得到 %u）", (unsigned)s_writes);
    CHECK(cq_persist_valid());
}

/** @brief setip 反例：任一字段非法 → 整包丢弃（不回显、不落盘、不改记录）
 *
 *  键名是协议原文的 `wg`（不是常见拼写 `gw`）：上位机若按 `gw` 发，本机必须拒——
 *  静默接受会让"改 IP 成功但网关没改"变成现场幽灵故障（设备能 ping 通本网段，
 *  跨网段全丢）。 */
static void case_setip_rejects(void)
{
    TEST_BEGIN("setip 反例：非法字段/错键名/越界端口 → 不回、不写、记录不变");

    fixture_reset();

    static const char *bad[] = {
        "{\"cmd\":\"setip\",\"ip\":\"256.1.1.1\",\"mask\":\"255.255.255.0\",\"wg\":\"192.168.1.1\",\"port\":20102}",
        "{\"cmd\":\"setip\",\"ip\":\"1.2.3\",\"mask\":\"255.255.255.0\",\"wg\":\"192.168.1.1\",\"port\":20102}",
        "{\"cmd\":\"setip\",\"ip\":\"192.168.1.77\",\"mask\":\"255.255.255\",\"wg\":\"192.168.1.1\",\"port\":20102}",
        /* 键名 gw 而非 wg */
        "{\"cmd\":\"setip\",\"ip\":\"192.168.1.77\",\"mask\":\"255.255.255.0\",\"gw\":\"192.168.1.1\",\"port\":20102}",
        /* 端口下界/上界越 1 */
        "{\"cmd\":\"setip\",\"ip\":\"192.168.1.77\",\"mask\":\"255.255.255.0\",\"wg\":\"192.168.1.1\",\"port\":0}",
        "{\"cmd\":\"setip\",\"ip\":\"192.168.1.77\",\"mask\":\"255.255.255.0\",\"wg\":\"192.168.1.1\",\"port\":65536}",
        /* ip 不是字符串 */
        "{\"cmd\":\"setip\",\"ip\":192,\"mask\":\"255.255.255.0\",\"wg\":\"192.168.1.1\",\"port\":20102}",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        cq_handle_frame(msg_new(bad[i], (uint16_t)strlen(bad[i])));

    CHECK_MSG(s_tx_calls == 0, "非法 setip 不得回显（得到 %d 次）", s_tx_calls);
    CHECK_MSG(s_writes == 0, "非法 setip 不得落盘（得到 %u 次）", (unsigned)s_writes);
    CHECK_MSG(s_cq.netcfg.magic != CQ_NETCFG_MAGIC, "记录不得被写（魔数 %08X）",
              (unsigned)s_cq.netcfg.magic);
    CHECK(!s_netcfg_valid);
}

/** @brief setip 落盘失败：**仍然原帧回显**（现状哨兵 —— 回显 ≠ 落盘成功）
 *
 *  上位机只能从回显判断"帧格式与字段合法"，判断不了 Flash 是否写成功。写失败时
 *  本机既不置 s_netcfg_valid，也不在运行态生效，但回显照发。
 *  现场症状：改 IP 后回显正常，**重启却仍是旧 IP**，且没有任何错误提示 —— 排查会
 *  先去怀疑网线/交换机，而不是"那一次没写进去"。
 *  另注（同一条记录的副作用）：记录是整体重写的，所以写失败后若接着来一次 picset
 *  落盘，这次没写成的网配会跟着那幅图一起被写进 Flash，重启后又"莫名其妙好了"。
 *
 *  **现状哨兵**：若协议方要求失败时回显里带错误码/回 NACK，本断言与 case_setip
 *  一并改；§10 的 O7 登记着这条口径待定。 */
static void case_setip_write_fail(void)
{
    TEST_BEGIN("setip 写失败：回显照发（回显≠落盘），记录未生效");

    fixture_reset();
    s_write_fail = true;

    const char *js = "{\"cmd\":\"setip\",\"ip\":\"192.168.1.99\",\"mask\":\"255.255.255.0\","
                     "\"wg\":\"192.168.1.1\",\"port\":20109}";
    uint16_t    n  = (uint16_t)strlen(js);

    cq_handle_frame(msg_new(js, n));

    CHECK_MSG(s_writes == 1, "应尝试写一次（得到 %u）", (unsigned)s_writes);
    CHECK_MSG(s_tx_calls == 1 && s_tx_len == n, "写失败仍回显（得到 %d 次 / %u 字节）", s_tx_calls,
              (unsigned)s_tx_len);
    CHECK_MSG(!s_netcfg_valid, "写失败不得把配置标成有效");
    CHECK_MSG(s_cq.netcfg.port == 20109, "RAM 里的候选值可以已更新（落盘才算数）");

    /* 写成功后同一份配置能真正生效：确认上面的失败没有把记录弄脏到不可恢复 */
    s_write_fail = false;
    tx_reset();
    cq_handle_frame(msg_new(js, n));
    CHECK(s_netcfg_valid);
    CHECK(s_tx_calls == 1);
}

/* ================================================================
 *  持久化与上电应用
 * ================================================================ */

/** @brief 固化记录装载：整条一个 CRC，坏一条即全丢（空记录而非半幅图）
 *
 *  记录是 7716B、跨两个 4K 扇区，没有逐槽 CRC —— 中途掉电必坏。设计的取舍是
 *  "宁可从空开始"：坏记录被整条复位后，三幅图都是未固化，屏上清空；若按部分
 *  有效装载，会把混着旧数据与新数据的图当成真的显示出去。
 *  装载后要为每槽建指纹，否则重启后第一次 8 段齐会白写一遍。 */
static void case_persist_load(void)
{
    TEST_BEGIN("固化记录：擦除态→空记录、有效记录读回并建指纹、损坏记录整条丢弃");

    /* 1) 擦除态（全 0xFF）= 首次上电 → 空记录 */
    fixture_reset();
    CHECK(s_cq.magic == CQ_PERSIST_MAGIC);
    CHECK(s_cq.slot[0].color == 0xFF && s_cq.slot[1].color == 0xFF && s_cq.slot[2].color == 0xFF);
    CHECK(!s_netcfg_valid);

    /* 2) 写一条有效记录 → 清 RAM（模拟掉电）→ 重新装载 */
    uint8_t vals[CQ_BITMAP_SIZE];
    for (size_t i = 0; i < sizeof(vals); i++) vals[i] = (uint8_t)(i * 13U + 1U);

    s_cq.slot[0].color = DEV_DISPLAY_COLOR_RED;
    memcpy(s_cq.slot[0].bitmap, vals, sizeof(vals));
    CHECK(cq_persist_write());

    uint32_t writes_before = s_writes;
    uint32_t reads_before  = s_reads;
    cq_persist_reset(); /* 清 RAM */
    cq_persist_load();

    CHECK(s_cq.slot[0].color == DEV_DISPLAY_COLOR_RED);
    CHECK_MSG(memcmp(s_cq.slot[0].bitmap, vals, sizeof(vals)) == 0, "位图应原样读回");
    CHECK_MSG(s_flushed_crc[0] != 0, "装载后应建好槽指纹（跨重启去重的前提）");
    CHECK_MSG(s_writes == writes_before, "装载只读不写（写次数 %u→%u）", (unsigned)writes_before,
              (unsigned)s_writes);
    CHECK(s_reads > reads_before);

    /* 3) 记录损坏（翻一个字节）→ 整条丢弃 */
    s_flash[s_persist_addr + 100] ^= 0xFF;
    cq_persist_reset();
    cq_persist_load();
    CHECK_MSG(s_cq.slot[0].color == 0xFF, "坏记录应整条丢弃（槽色 %02X）", s_cq.slot[0].color);
    CHECK(s_cq.slot[0].bitmap[0] == 0);
}

/** @brief 上电应用：无配置用默认端口；有配置才改 IP；IP 相同不重复 set
 *
 *  pl_net_set_ip 会触发 IP 变更回调（其它模块跟着对账），重复调用没有意义还可能
 *  让对账逻辑多跑一轮；所以"值相同就跳过"是刻意行为。端口必须早于 UDP 首绑生效，
 *  否则第一包上位机数据就发到旧端口上去了。 */
static void case_net_apply(void)
{
    TEST_BEGIN("上电应用：默认端口 / 应用记录 / 值相同不重复 set / 应答跟随端口");

    fixture_reset();

    /* 无有效记录 → 套**本产品出厂 IP 192.168.1.10**（不是 dev_eth 的 1.100）+ 默认端口。
       这条是产品口径：出厂不带记录时，屏必须落在 1.10 上（上位机按这个地址找它）。 */
    cq_net_apply();
    CHECK_MSG(s_net_set_calls == 1, "无记录应把网卡设成出厂 IP（得到 %d 次 set）", s_net_set_calls);
    CHECK_MSG(memcmp(s_net_ip, (const uint8_t[]){192, 168, 1, 10}, 4) == 0, "出厂 IP 应为 1.10");
    CHECK(memcmp(s_net_mask, (const uint8_t[]){255, 255, 255, 0}, 4) == 0);
    CHECK(memcmp(s_net_gw, (const uint8_t[]){192, 168, 1, 1}, 4) == 0);
    CHECK_MSG(s_udp_port_calls == 1 && s_udp_port == CQ_UDP_PORT_DEFAULT,
              "无配置应绑默认端口 %u（得到 %u）", (unsigned)CQ_UDP_PORT_DEFAULT,
              (unsigned)s_udp_port);
    CHECK(s_cq_port == CQ_UDP_PORT_DEFAULT);

    /* 再上电一次：运行态已经是 1.10 → 不得重复下发（pl_net_set_ip 会触发 IP 变更回调） */
    cq_net_apply();
    CHECK_MSG(s_net_set_calls == 1, "已是出厂值不得重复 set（得到 %d）", s_net_set_calls);

    /* 存一条与运行态不同的配置 → **记录优先**，不能被出厂值盖回去 */
    CHECK(cq_netcfg_save((const uint8_t[]){192, 168, 1, 88}, (const uint8_t[]){255, 255, 255, 0},
                         (const uint8_t[]){192, 168, 1, 254}, 20123));
    net_stub_reset();

    cq_net_apply();
    CHECK_MSG(s_net_set_calls == 1, "应应用一次 IP（得到 %d）", s_net_set_calls);
    CHECK(memcmp(s_net_ip, (const uint8_t[]){192, 168, 1, 88}, 4) == 0);
    CHECK(memcmp(s_net_gw, (const uint8_t[]){192, 168, 1, 254}, 4) == 0);
    CHECK_MSG(s_udp_port == 20123, "端口应取自记录（得到 %u）", (unsigned)s_udp_port);
    CHECK(s_cq_port == 20123);

    /* 再应用一次：与运行态相同 → 不再 set */
    cq_net_apply();
    CHECK_MSG(s_net_set_calls == 1, "值相同不得重复 set（得到 %d）", s_net_set_calls);
    CHECK(s_udp_port == 20123);

    /* 应答目的端口 = 本机监听端口（上层改过端口后也跟着走） */
    tx_reset();
    cq_reply_ok(msg_new("{\"cmd\":\"picset\"}", 16));
    CHECK_MSG(s_tx_calls == 1 && s_tx_port == 20123, "应答应发往 %u（得到 %u）", 20123U,
              (unsigned)s_tx_port);
}

/** @brief 干接点显示：未固化槽只清屏，已固化槽黑底 + 位图
 *
 *  SW1/SW2/SW3 与三幅固化图一一对应。槽未固化时若还把 0xFF 的颜色当色号去画，
 *  屏上会出杂色噪声；清屏是现场能理解的"这一路没配图"。 */
static void case_pic_show(void)
{
    TEST_BEGIN("干接点显示：未固化只清屏、已固化黑底+位图、越界槽不崩");

    fixture_reset();

    /* 未固化槽 */
    cq_pic_show(0);
    CHECK_MSG(s_fill_calls == 1 && s_fill_color == DEV_DISPLAY_COLOR_BLACK, "应先清屏");
    CHECK_MSG(s_draw_calls == 0, "未固化槽不得画位图（得到 %d）", s_draw_calls);

    /* 固化槽 1 */
    s_cq.slot[1].color = DEV_DISPLAY_COLOR_RED;
    memset(s_cq.slot[1].bitmap, 0xA5, CQ_BITMAP_SIZE);
    cq_pic_show(1);

    CHECK(s_fill_calls == 2);
    CHECK_MSG(s_draw_calls == 1, "已固化槽应画一次位图（得到 %d）", s_draw_calls);
    CHECK_MSG(s_draw_ptr == s_cq.slot[1].bitmap, "画的应是该槽的位图");
    CHECK(s_draw_color == DEV_DISPLAY_COLOR_RED);
    CHECK(s_draw_w == TEST_PANEL_ROWS && s_draw_h == TEST_PANEL_COLS);

    /* 越界槽号：只清屏，不越界读 */
    cq_pic_show(CQ_PIC_COUNT);
    CHECK(s_fill_calls == 3);
    CHECK(s_draw_calls == 1);
}

/* ================================================================
 *  贯通：字节流 → 探针 → 命令层 → 应答
 * ================================================================ */

/** @brief 端到端：真字节流经探针切帧、按帧长取走、再交给命令层
 *
 *  单测里各段都绿、合起来仍可能错：探针报的长度与命令层期望的 data_len 必须一致
 *  （短了 JSON 被截断、长了把下一帧的头带进 JSON）。本用例走一遍"收 → 切 → 取 →
 *  处理 → 回"，把三段接缝钉住；顺带确认未知命令与非 JSON 一律静默丢弃。 */
static void case_handle_frame_dispatch(void)
{
    TEST_BEGIN("贯通：探针切帧 → 取帧 → 命令层 → 应答；未知命令静默丢弃");

    fixture_reset();

    uint8_t vals[CQ_PACKET_PAYLOAD];
    fill_pattern(vals, 0x40);

    size_t n = frame_picset(s_frame, 2, 'r', 5, vals, CQ_PACKET_PAYLOAD);
    feed(s_frame, (uint16_t)n);

    CHECK(probe(sizeof(s_scratch)) == APP_PCB_PROBE_STATE_READY);
    CHECK_MSG(s_probe_len == n, "探针帧长应与构造长度一致（%u vs %u）", (unsigned)s_probe_len,
              (unsigned)n);

    /* 模拟 dispatch：按探针报的长度取走这一帧 */
    static uint8_t got[2048] __attribute__((aligned(4)));
    uint16_t       got_len = rb_read(&s_cq_rb, got, (uint16_t)s_probe_len, nullptr);
    CHECK_MSG(got_len == n, "取帧长度应为 %u（得到 %u）", (unsigned)n, (unsigned)got_len);
    CHECK(rb_avail(&s_cq_rb, nullptr) == 0);

    cq_handle_frame(msg_new((const char *)got, got_len));
    CHECK_MSG(s_tx_calls == 1 && s_tx_len == 25, "贯通后应回一帧 OK（len=%u）", (unsigned)s_tx_len);
    CHECK(s_cq.slot[2].color == DEV_DISPLAY_COLOR_RED);
    CHECK(memcmp(&s_cq.slot[2].bitmap[4U * CQ_PACKET_PAYLOAD], vals, CQ_PACKET_PAYLOAD) == 0);

    /* 未知 cmd（含只差一个字符的 picsett）→ 不应答 */
    static const char *unknown[] = {
        "{\"cmd\":\"hello\",\"nu\":1}",
        "{\"cmd\":\"picsett\",\"nu\":1,\"c\":\"g\"}",
        "{\"cmd\":\"setipx\"}",
        "not a json at all",
    };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++)
        cq_handle_frame(msg_new(unknown[i], (uint16_t)strlen(unknown[i])));

    CHECK_MSG(s_tx_calls == 1, "未知命令不得应答（得到 %d 次）", s_tx_calls);
    CHECK(s_cq.slot[2].color == DEV_DISPLAY_COLOR_RED); /* 未被改 */
}

/* ================================================================ */

int main(void)
{
    printf("CQ_P10 协议 host 单测\n");

    case_probe_states();
    case_probe_brace_balance();
    case_probe_limits();
    case_json_key_lookup();
    case_json_str_parse();
    case_json_int_parse();
    case_parse_ipv4();
    case_parse_t_value();
    case_json_whitespace_crlf();
    case_picset_segment();
    case_picset_rejects();
    case_picset_flush_dedupe();
    case_picset_fill_all();
    case_setip();
    case_setip_rejects();
    case_setip_write_fail();
    case_persist_load();
    case_net_apply();
    case_pic_show();
    case_handle_frame_dispatch();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
