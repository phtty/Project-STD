/**
 * @file    pl_flash_stub.c
 * @brief   pl_flash 的 host 替身 —— RAM 里的假内部 Flash
 *
 * 为什么需要它：IAP 记录的两个缺陷（并发写交错、损坏后永久无法修复）都由
 * **编程中途掉电**触发，而掉电时序在真机上构造不出来。这里把内部 Flash 换成
 * 一块 RAM，于是"擦到一半"、"写到第 N 个 word 掉电"都成了可控输入。
 *
 * 刻意复刻的两条 NOR 语义（桩若比真硬件"宽容"，测试会掩盖真缺陷）：
 *   1. 编程只能把 1 写成 0 —— 对同一地址重复编程（中间没擦除）会失败。
 *      真硬件上它的表现是"数据悄悄变成两个值的按位与"，这里直接失败更好定位。
 *   2. 擦除的最小单位是整个扇区 —— dev_flash_int._erase 忽略 addr/len 也是
 *      这个原因（一实例一扇区），桩同样整块擦。
 *
 * 故障注入与交错检测是为被测的两个缺陷量身做的，见 test_iap_cfg.c。
 */

#include "pl_flash.h"

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t s_base;
static uint32_t s_len;

static int  s_erase_cnt;
static int  s_prog_cnt;
/* 故障注入按**相对注入点**计数，不是累计次数 —— 调用方关心的永远是"从现在起
   再写几个 word 就掉电"。用累计值的话，注入时计数早已非零，一比较就立刻全失败，
   得到的是"一个 word 都没写"（空记录）而不是"写了一半"（损坏记录）。 */
static int  s_fail_n    = -1; /* <0 = 不注入；否则注入点之后再写 s_fail_n 个 word 就失败 */
static int  s_fail_base;      /* 注入时的编程计数 */
static bool s_interleaved;
static int  s_seq_prog;        /* 本次擦除之后已编程的 word 数 */
static int  s_expect_words = 1; /* 一次完整写序列的 word 数；<=0 表示不检测 */
static int  s_yield_every;      /* >0 = 每编程这么多个 word 让出一次 CPU */

/* ---- 测试侧控制 ---- */

void pl_flash_stub_set_region(uint32_t base, uint32_t len)
{
    s_base = base;
    s_len  = len;
}

void pl_flash_stub_reset(void)
{
    s_erase_cnt   = 0;
    s_prog_cnt    = 0;
    s_fail_n      = -1;
    s_interleaved = false;
    s_seq_prog    = 0;
    s_yield_every = 0;
}

/** @brief 每编程 n 个 word 让出一次 CPU，用于把并发交错变成必现
 *
 *  不注入的话两个线程也能撞上，但靠调度器撞、时快时慢；注入之后
 *  "不加锁就会交错"从概率事件变成确定事件，测试才有意义。 */
void pl_flash_stub_set_yield_every(int n) { s_yield_every = n; }

/** @brief 从第 n 次编程开始全部失败（模拟编程到一半掉电）
 *
 *  n=0 表示第一次编程就失败（擦除已完成、一个 word 都没写进去）。
 *  真机上的对应现象：擦除后掉电 → 记录全 0xFF（空）；写了一个 word 后掉电
 *  → magic 已写、config_crc 仍是 0xFFFFFFFF（既非空也非有效，即"损坏"）。 */
void pl_flash_stub_fail_after(int n)
{
    s_fail_base = s_prog_cnt;
    s_fail_n    = n;
}

/** @brief 告知桩"一次完整写序列有几个 word"，用于交错检测
 *
 *  序列边界桩自己推不出来（它只看到一串 program_word），由测试告知。 */
void pl_flash_stub_expect_words(int n) { s_expect_words = n; }

int  pl_flash_stub_erase_count(void) { return s_erase_cnt; }
int  pl_flash_stub_program_count(void) { return s_prog_cnt; }

/** @brief 是否观察到"擦除打断了一个未完成的编程序列"
 *
 *  这正是并发写不加锁时的坏交错：A 擦除并写了前几个 word，B 擦除把它们抹掉，
 *  B 写完自己的 17 个 word 后 A 又接着写剩下的 word —— 最终记录前几个 word 来自
 *  B、后几个来自 A，CRC 必然对不上。加了锁就不可能观察到。 */
bool pl_flash_stub_interleaved(void) { return s_interleaved; }

/* ---- pl_flash 接口 ---- */

void pl_flash_unlock(void) {}
void pl_flash_lock(void) {}
void pl_flash_clear_errors(void) {}

int32_t pl_flash_erase_sector(pl_flash_sector_t sector, pl_flash_voltage_t range)
{
    (void)sector;
    (void)range;

    if (s_expect_words > 0 && s_seq_prog > 0 && s_seq_prog < s_expect_words)
        s_interleaved = true;

    memset((void *)(uintptr_t)s_base, 0xFF, s_len);
    s_erase_cnt++;
    s_seq_prog = 0;
    return 0;
}

int32_t pl_flash_program_word(uint32_t addr, uint32_t data)
{
    /* 掉电注入：计数照走（调用方看到的是"这次编程没成功"），但不写内存 */
    if (s_fail_n >= 0 && (s_prog_cnt - s_fail_base) >= s_fail_n) {
        s_prog_cnt++;
        return -1;
    }

    uint32_t *p = (uint32_t *)(uintptr_t)addr;

    /* NOR 语义：只能把 1 写成 0。中间没擦除就重复编程会失败 —— 真硬件上是
       "数据变成两个值的按位与"，这里直接失败更好定位。 */
    if ((*p & data) != data) return -1;

    *p = data;
    s_prog_cnt++;
    s_seq_prog++;

    if (s_yield_every > 0 && (s_prog_cnt % s_yield_every) == 0) sched_yield();
    return 0;
}
