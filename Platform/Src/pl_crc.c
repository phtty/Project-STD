/**
 * @file        pl_crc.c
 * @brief       CRC 硬件抽象（STM32 硬件 CRC32 单元）
 *
 * 封装 STM32F4 硬件 CRC32 单元（多项式 0x4C11DB7）。输入对齐时零拷贝直通，
 * 否则分块搬进对齐缓冲累加 —— 见 `_crc32_unaligned` 的说明。
 */

#include "pl_crc.h"
#include "crc.h"
#include "cmsis_os2.h"
#include "initcall.h"
#include <string.h>

/* ================================================================
 *  互斥
 *
 *  **hcrc 是共享外设，而调用方分散在不同任务**：IAP 探针在**分发任务**里算帧 CRC，
 *  `app_iap_cfg` 在 IAP/LDI 任务里算记录 CRC，级联协议又会在分发任务里算位图帧。
 *  这些任务同为 Normal 优先级 —— tick 抢占就能让两次计算交错，而 CRC 单元是**有状态
 *  的**（上一次的结果是下一次的输入），交错的结果是两边都算错。
 *
 *  症状是"CRC 偶尔对不上"，在 1ms 的 tick 里窗口只有几十微秒，现场会表现为极低频的
 *  协议校验失败，最难查的一类。加一把锁的成本是一次 acquire/release。
 *
 *  锁在 sw_pl(1) 建：hw_pl 阶段建内核对象会出事（见 pl_tim.c 的说明）。
 *  RTOS 未就绪时不加锁 —— 那是单线程启动期，不存在竞争。 */
static osMutexId_t s_crc_lock;

static void _crc_lock_init(void)
{
    const osMutexAttr_t attr = {.name = "crc", .attr_bits = osMutexPrioInherit};
    s_crc_lock               = osMutexNew(&attr);
}
sw_pl_initcall(_crc_lock_init);

void pl_crc_init(void)
{
    MX_CRC_Init();
}
hw_pl_initcall(pl_crc_init);

pl_crc_handle_t pl_crc_get_handle(void)
{
    return (pl_crc_handle_t)&hcrc;
}

/* ================================================================
 *  计算
 * ================================================================ */

/** @brief 分块搬进对齐缓冲、逐块累加 —— 任意长度、任意对齐、**无长度上限**
 *
 *  **为什么不能直接用 HAL_CRC_Calculate**：它只接受 `uint32_t *`，非对齐输入必须先
 *  拷进对齐缓冲。原实现在这里放了个 `uint32_t buf[64]` 的**栈缓冲**，而且**只算一次**
 *  —— 于是超过 256 字节的非对齐输入被**静默截断**（只剩前 256 字节参与校验；
 *  发收两侧用的是同一个函数、算出来还一致，所以不报任何错）。
 *
 *  而 1KB 级的非对齐输入是完全正常的用法：级联协议的帧头 11 字节，CRC 覆盖的区段
 *  从偏移 2 开始 —— 暂存区本身 4 字节对齐，+2 就不对齐了，**必走这条路**。
 *
 *  修法：**分块累加**。`HAL_CRC_Accumulate` 不重置 DR，可以接着上一次继续算，
 *  于是把输入切成 64 字（256 字节）一块逐块喂，缓冲仍然是栈上的 256 字节，
 *  但整段都会被算到。块的边界上不补零（256 是 4 的倍数），只有**最后一块**
 *  的尾巴按小端补零 —— 与原来那条路径同一约定，故对"长度为 4 的倍数的对齐输入"
 *  两种路径结果逐位一致，现有调用方（IAP 帧校验、IAP 记录 CRC）行为不变。
 *
 *  不用"直接写 DR 寄存器"的写法：那样更快一点，但 Platform 层就得自己戳寄存器，
 *  而且**没法在 host 上测**（C 拦不住寄存器写，桩做不出那个语义）。
 *  实测这点差别无关紧要 —— 瓶颈是 RS485 的线，不是这几十微秒。 */
static uint32_t _crc32_unaligned(const uint8_t *data, size_t len)
{
    uint32_t buf[64];
    bool     started = false;

    while (len) {
        size_t take = len > sizeof(buf) ? sizeof(buf) : len;

        memset(buf, 0, sizeof(buf)); /* 末块的尾巴靠这一步补零 */
        memcpy(buf, data, take);

        uint32_t words = (uint32_t)((take + 3U) / 4U);
        if (started)
            HAL_CRC_Accumulate(&hcrc, buf, words); /* 接着上一次算 */
        else
            HAL_CRC_Calculate(&hcrc, buf, words); /* 第一块负责复位到初值 */

        started = true;
        data += take;
        len -= take;
    }

    /* len == 0：一次都没算过，而 DR 里是**上一次计算**的残留值。必须显式复位 ——
       原实现在这条路上调的正是 HAL_CRC_Calculate（它会复位），所以结果是初值；
       改成分块累加后漏掉这一步就变成了"返回别人的 CRC"，而它只在
       "空输入 + 指针不对齐"时出现，极难在生产里撞到、也极难归因。
       （这条是 host 单测抓出来的。） */
    if (!started) return HAL_CRC_Calculate(&hcrc, buf, 0);

    return hcrc.Instance->DR;
}

uint32_t pl_crc32_calc(pl_crc_handle_t h, const uint8_t *data, size_t len)
{
    (void)h;
    if (!data) return 0xFFFFFFFFU;

    bool locked = false;
    if (s_crc_lock && osMutexAcquire(s_crc_lock, osWaitForever) == osOK) locked = true;

    uint32_t r;
    if (((uintptr_t)data & 3U) == 0U && (len & 3U) == 0U) {
        /* 对齐且整字：零拷贝直通，走 HAL —— 这条是现有调用方一直在走的路，
           保持原样把"改这个函数会不会打坏 IAP"的风险降到零 */
        r = HAL_CRC_Calculate(&hcrc, (uint32_t *)(uintptr_t)data, (uint32_t)(len / 4U));
    } else {
        r = _crc32_unaligned(data, len);
    }

    if (locked) osMutexRelease(s_crc_lock);
    return r;
}
