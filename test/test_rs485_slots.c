/**
 * @file    test_rs485_slots.c
 * @brief   RS485 收包槽位：环回拆帧两段都要接住，投递失败必须把槽还回去
 *
 * **为什么需要这个测试**：这两条都是**静默**失效，现场表现为"突然什么都收不到了"，
 * 而寄存器、状态机、日志都看不出任何异常 —— 上机排查一轮的代价是几天。
 *
 *   1. **环回拆帧**：`uart_idle_handle` 在接收缓冲环回处会把一段拆成**两次背靠背的回调**
 *      （先交缓冲末尾那一截、再交开头那一截）。也就是"一次交付"最多同时占 2 个槽，
 *      而任务手上最多还握着 1 个 —— 槽位只有 2 个时**必然**丢掉后半段，整帧作废。
 *      实测：一条 1427 字节的帧跨过回绕点被拆成 1211 + 216，216 被丢，只能靠主卡重发。
 *   2. **投递失败的槽会永久漏掉**：队列深度小于槽数时，`osMessageQueuePut` 会失败
 *      （超时 0），而 busy 位已经置上 —— 那个槽再没人会来取。漏够槽数就**永久哑掉**，
 *      且一声不响。实测日志里"某一段没有对应的交付行"就是它。
 *
 * 用例 TU-include 生产源码（`rs485_isr_cb` 与槽位都是 static），只把 HAL/总线换成替身；
 * 队列用**生产的那份属性**（`s_rs485_rx_attr`），所以缓冲定容写错也会被抓到。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_rs485.h"
#include "app_dispatch.h"
#include "ring_buffer.h"
#include "pl_mem.h"  /* pl_mem_is_dma_capable（inline，要用到段边界符号） */
#include "pl_uart.h" /* 替身要按真签名写 */
#include "pl_task.h"

/* 段边界由链接脚本在真机上定义；host 上没有链接脚本，给一对空区间
   （任何缓冲都不落在 CCMRAM 里）—— 本套件不测那一段。 */
uint8_t _sccmram[1];
uint8_t _eccmram[1];

/* ---- 替身：发送路径与通道启动 ----
 * 本套件只测**收包槽位**，但这些符号和被测函数在同一个 TU 里（gc-sections 不会
 * 把 rs485_send 摘掉 —— 它被 ops 表引用），所以得给出定义让整体编得过。 */
int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)h;
    (void)buf;
    (void)timeout_ms;
    return (int32_t)len;
}
int32_t pl_uart_send_dma(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)h;
    (void)buf;
    (void)timeout_ms;
    return (int32_t)len;
}
pl_uart_handle_t pl_uart_get_handle(uint8_t id)
{
    (void)id;
    return nullptr;
}
void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx)
{
    (void)h;
    (void)cb;
    (void)ctx;
}
int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len)
{
    (void)h;
    (void)buf;
    (void)len;
    return 0;
}
void *pl_task_new(void (*fn)(void *), void *arg, const osThreadAttr_t *attr)
{
    (void)fn;
    (void)arg;
    (void)attr;
    return nullptr;
}
void app_ccb_dispatch(const ccb_t *ccb, const ccb_src_t *src, const uint8_t *data, uint16_t len)
{
    (void)ccb;
    (void)src;
    (void)data;
    (void)len;
}

/* 被测：生产源码本体 */
#include "../Application/Src/Channel/app_rs485.c"

/* ================================================================
 *  夹具
 * ================================================================ */

static rs485_ccb_t s_self;

static void fixture_reset(void)
{
    memset(&s_self, 0, sizeof(s_self));
    s_slot_next = 0;
    s_slot_busy = 0;

    /* **走生产的那条建队列路径**（`_rx_queue_create`）：测试自己另建一个的话，
       生产里把深度改小了它也不会红 —— 那就是假信心。 */
    s_self.rx_queue = _rx_queue_create();
}

/** @brief 一段可辨认的数据：第 k 段全是 k，抄错/串段立刻看得出来
 *
 *  缓冲比 RS485_BUF_SIZE 大一点：本套件要能**真的**构造出"超长段"（+1 字节），
 *  夹到 2048 的话那条用例就成了空转。 */
static uint16_t feed_chunk(uint8_t tag, uint16_t len)
{
    static uint8_t buf[RS485_BUF_SIZE + 64U];

    if (len > sizeof(buf)) len = sizeof(buf);
    memset(buf, tag, len);
    rs485_isr_cb(buf, len, &s_self);
    return len;
}

/** @brief 从队列取一条，返回它的槽号；没有则 0xFF */
static uint8_t take_slot(void)
{
    rs485_rx_msg_t m = {0};
    if (osMessageQueueGet(s_self.rx_queue, &m, nullptr, 0) != osOK) return 0xFF;
    return m.slot;
}

/** @brief 取一条并按生产的顺序释放它（任务里就是"交出去之后才清 busy"）
 *  @return 该段的长度；没有可取返回 0xFFFF */
static uint16_t take_and_release(uint8_t *tag_out)
{
    const uint8_t slot = take_slot();
    if (slot == 0xFF) return 0xFFFF;

    const uint16_t n = s_slots[slot].len;
    if (n) *tag_out = s_slots[slot].data[0];
    s_slot_busy &= (uint8_t)~(1U << slot); /* 与 rs485_task 同序：交出去之后才释放 */
    return n;
}

/* ---- 断言 ---- */
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

/* ================================================================ */

/** 环回拆帧：两次背靠背的回调，两段都必须接住、顺序不能反 */
static void case_wrap_split_kept(void)
{
    TEST_BEGIN("环回拆帧：两段背靠背的回调都要接住（顺序不变）");

    fixture_reset();

    /* 1427 字节跨过回绕点 = 1211（到缓冲末尾）+ 216（绕回来的那截），
       这两次回调是**同一次空闲事件里背靠背**发生的 */
    feed_chunk(0xA1, 1211);
    feed_chunk(0xA2, 216);

    uint8_t  tag = 0;
    uint16_t n   = take_and_release(&tag);
    CHECK_MSG(n == 1211 && tag == 0xA1, "第一段应是 1211 字节的 0xA1，得到 %u 字节的 0x%02X",
              (unsigned)n, (unsigned)tag);

    n = take_and_release(&tag);
    CHECK_MSG(n == 216 && tag == 0xA2, "第二段应是 216 字节的 0xA2，得到 %u 字节的 0x%02X",
              (unsigned)n, (unsigned)tag);

    CHECK_MSG(take_slot() == 0xFF, "不该有第三段");
    CHECK_MSG(s_slot_busy == 0, "排空之后不该还有占着的槽：busy=%02X", (unsigned)s_slot_busy);
}

/** 槽位数至少要能覆盖"环回拆出 2 段 + 任务手上 1 个" */
static void case_slot_count_enough(void)
{
    TEST_BEGIN("槽位数覆盖最坏情形（任务手上 1 个 + 环回拆出 2 段）");

    CHECK_MSG(RS485_RX_SLOTS >= 3, "槽位只有 %u 个 —— 环回那一刻必然丢一段",
              (unsigned)RS485_RX_SLOTS);
}

/** 队列深度必须覆盖槽数 —— 少了就有一个槽投不进去，而它已被标成占用 → 永久漏
 *
 *  **这是那条"永久哑掉"的守门用例**：`osMessageQueueNew` 的深度若小于 `RS485_RX_SLOTS`，
 *  填满槽的那一轮里必有一次 `osMessageQueuePut` 失败；若失败时还把槽标成占用
 *  （旧代码就是：`busy |= ...` 之后不看返回值），那个槽就再没人取，漏够槽数即彻底收不到。
 *  **反向验证**：把 `osMessageQueueNew` 的深度改小，下面"排空应得 N 段"立刻红。
 *
 *  注：深度与槽数**相等**之后，`osMessageQueuePut` 其实再不会因满而失败（满了必然也
 *  没有空槽），所以源码里那条失败分支是**防御性**的；真正会走到的是**槽位耗尽**，
 *  下面第二段测它。 */
static void case_depth_covers_slots(void)
{
    TEST_BEGIN("队列深度覆盖槽数（少一个就会永久漏一个槽）");

    fixture_reset();

    /* 填满所有槽：每一段都必须进得了队列（深度不够的话会少一段） */
    for (uint8_t i = 0; i < RS485_RX_SLOTS; i++) feed_chunk((uint8_t)(0x10 + i), 15);

    uint8_t tag = 0;
    uint8_t got = 0;
    while (take_and_release(&tag) != 0xFFFF) got++;
    CHECK_MSG(got == RS485_RX_SLOTS, "排空应得 %u 段，得到 %u 段 —— 深度不够就会少",
              (unsigned)RS485_RX_SLOTS, (unsigned)got);
    CHECK_MSG(s_slot_busy == 0, "排空之后不该还有占着的槽：busy=%02X", (unsigned)s_slot_busy);

    /* 槽位耗尽（真会走到的那条）：多来一段就被丢，但**不能把状态搅坏** */
    for (uint8_t i = 0; i < RS485_RX_SLOTS; i++) feed_chunk((uint8_t)(0x20 + i), 15);
    feed_chunk(0xEE, 15); /* 此时已无空槽 */
    got = 0;
    while (take_and_release(&tag) != 0xFFFF) got++;
    CHECK_MSG(got == RS485_RX_SLOTS, "被丢的那段不该占住任何槽，排空后仍是 %u 段，得到 %u",
              (unsigned)RS485_RX_SLOTS, (unsigned)got);

    /* 之后还能正常收 */
    feed_chunk(0x5A, 15);
    CHECK_MSG(take_and_release(&tag) == 15 && tag == 0x5A, "槽位耗尽之后还能正常收包");
    CHECK_MSG(s_slot_busy == 0, "收完仍然不该有占着的槽：busy=%02X", (unsigned)s_slot_busy);
}

/** 超过缓冲容量的段不可能来自 DMA，但守卫在就不该被写出去 */
static void case_oversize_rejected(void)
{
    TEST_BEGIN("超过缓冲容量的段被拒（不得越界写槽）");

    fixture_reset();
    feed_chunk(0x77, (uint16_t)(RS485_BUF_SIZE + 1));
    CHECK_MSG(take_slot() == 0xFF, "超长段不该进队列");

    /* 后续正常段不受影响 */
    feed_chunk(0x11, 15);
    uint8_t tag = 0;
    CHECK_MSG(take_and_release(&tag) == 15 && tag == 0x11, "超长段之后仍应正常收包");
}

/* ================================================================ */

int main(void)
{
    printf("\n\033[36mRS485 收包槽位（%u 个 × %u 字节）\033[0m\n", (unsigned)RS485_RX_SLOTS,
           (unsigned)RS485_BUF_SIZE);

    case_slot_count_enough();
    case_wrap_split_kept();
    case_depth_covers_slots();
    case_oversize_rejected();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
