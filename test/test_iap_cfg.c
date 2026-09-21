/**
 * @file    test_iap_cfg.c
 * @brief   IAP 记录（内部 Flash Sector 1）的 host 单测
 *
 * 为什么需要这个测试：这条记录的两个缺陷都**只有在真机上极难构造的时序下**
 * 才现形 ——
 *
 *   ① 并发写不加锁时，两条任务的"擦除 + 编程 17 个 word"序列会逐 word 交错，
 *      拼出前几个 word 来自一方、后几个来自另一方的记录，CRC 必然对不上。
 *   ② 编程中途掉电会留下"既非空也非有效"的记录（magic 是第 0 个 word、
 *      config_crc 是最后一个，写一半掉电正好卡在中间）。原先的实现对损坏记录
 *      直接 return 且全工程没有别的修复入口 —— 现场唯一出路是整片重烧。
 *
 * 掉电时序没法在真机上复现，所以在 test/stubs/pl_flash_stub.c 里把内部 Flash
 * 换成了 RAM：擦到一半、写到第 N 个 word 掉电、两线程强制交错都成了可控输入。
 *
 * 被测代码是生产源码本体（Application/Src/IAP/app_iap_cfg.c +
 * Device/Storage/dev_flash_int.c），不做任何替换；重定向只通过覆盖 g_config
 * 与存储实例的 base_addr 完成（见 flash_setup）。
 *
 * 构建与运行见 Makefile 的 test 目标。
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_iap_cfg.h"
#include "dev_flash_int.h"

/* 直接 include 实现 TU：并发用例需要调用文件内 static 的 _iap_cfg_lock_init()。
 * host 上 sw_dev_initcall 不会被执行（initcall_run 只在固件里跑），s_lock 就一直是
 * NULL，各入口的空守卫等于没锁 —— 并发用例会（正确地）报出交错。生产代码不动，
 * 由测试显式补上这一步，与 test_cfg_sched.c 对 app_ldi_cfg.c 的做法一致。
 * 注意 Makefile 里**不能**再列 Application/Src/IAP/app_iap_cfg.c，否则符号重定义。 */
#include "../Application/Src/IAP/app_iap_cfg.c"

/* ---- pl_flash 替身的控制接口（test/stubs/pl_flash_stub.c）---- */
void pl_flash_stub_set_region(uint32_t base, uint32_t len);
void pl_flash_stub_reset(void);
void pl_flash_stub_fail_after(int n);
void pl_flash_stub_expect_words(int n);
void pl_flash_stub_set_yield_every(int n);
int  pl_flash_stub_erase_count(void);
int  pl_flash_stub_program_count(void);
bool pl_flash_stub_interleaved(void);

/* ---- 生产代码依赖、本测试不关心的两个接口 ---- */
void pl_net_get_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
    const uint8_t z[4] = {0, 0, 0, 0};
    memcpy(ip, z, 4);
    memcpy(mask, z, 4);
    memcpy(gw, z, 4);
}
uint16_t app_tcp_server_get_port(void) { return 9529; }

/* ---- 极简断言 ---- */
static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* __VA_OPT__ 让本宏在"不带可变参数"时也能用（C23）。
   直接写 __VA_ARGS__ 的话，CHECK_MSG(c, "msg") 会展开成 printf(..., ) 编译不过 ——
   而"只有一句结论、没有附加数值"恰恰是最常见的用法。 */
#define CHECK_MSG(cond, fmt, ...)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__);     \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* ---- 假记录区 ----
 *
 * **必须落在 32 位可寻址区内**：dev_flash_int_t.base_addr 是 uint32_t，而 _read 是
 * `memcpy(buf, (void *)(self->base_addr + addr), len)` —— 在 64 位宿主机上，
 * 把普通指针塞进 uint32_t 会被截断，读到的是一块无关内存，测试会以"数据不对"
 * 的形式静默失败而不是报错。MAP_32BIT 拿到的地址 < 2^32，截断可逆。
 *
 * 代价：MAP_32BIT 是 Linux/x86-64 专有。换平台时这里会编译/运行失败，属可接受的
 * 失败方式（比静默读到错内存好）。生产代码的 uint32_t 在 32 位目标上是正确的，
 * 不要为了 host 测试把它改成 uintptr_t。 */
#define FAKE_SECTOR_SIZE 4096

static uint8_t *g_flash;

#define REC ((app_flash_iap_sys_info_t *)(uintptr_t)(uint32_t)(uintptr_t)g_flash)

/** @brief 把记录区重定向到 RAM，并把存储实例指向同一块
 *
 *  生产代码里两者都是 0x08004000 —— g_config 走内存映射读、dev_flash_int 走
 *  base_addr 读，改完 ip 之后两条路径必须落到同一块内存，否则测试自欺。 */
static void flash_setup(void)
{
    if (!g_flash) {
        void *p = mmap(NULL, FAKE_SECTOR_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) {
            printf("mmap(MAP_32BIT) 失败 —— 本测试需要 32 位可寻址的假 Flash\n");
            exit(2);
        }
        g_flash = p;
    }
    memset(g_flash, 0xFF, FAKE_SECTOR_SIZE);

    uint32_t base = (uint32_t)(uintptr_t)g_flash; /* MAP_32BIT 保证不截断 */

    dev_flash_int_t *st = (dev_flash_int_t *)app_flash_iap_get_storage();
    st->base_addr       = base;
    st->me.ops          = &flash_int_ops;

    g_config = (app_flash_iap_sys_info_t *)(uintptr_t)base;

    pl_flash_stub_set_region(base, FAKE_SECTOR_SIZE);
    pl_flash_stub_reset();

    /* 补上固件里由 sw_dev_initcall 做的那一步（幂等） */
    if (!s_lock) _iap_cfg_lock_init();
    /* 一次完整写序列 = 记录的 word 数，交错检测据此判断序列边界 */
    pl_flash_stub_expect_words((int)(sizeof(app_flash_iap_sys_info_t) / 4));
}

static const uint8_t IP_A[4]    = {10, 0, 0, 1};
static const uint8_t IP_B[4]    = {10, 0, 0, 2};
static const uint8_t MASK[4]    = {255, 255, 255, 0};
static const uint8_t GW[4]      = {10, 0, 0, 254};
#define PORT 9529

/* ================================================================
 *  分板：本板有没有 IAP 记录区
 *
 *  **下面那一整套用例只对"带 bootloader、Sector 1 是记录区"的板成立。**
 *  直烧板（BOARD_HAS_IAP_RECORD 0）的 0x08004000 落在固件映像内部，app_iap_cfg.c
 *  的擦/写两个最底层原语被整体短路，所有建立在"能擦能写"之上的用例在这块板上
 *  没有意义 —— 强行跑只会得到一片假失败。
 *
 *  本板真正要守的是**短路本身**：没有它，启动对账会判定"记录无效"→ 擦 Sector 1
 *  → 擦掉的正是正在执行的代码 → 每次上电必崩（上机 HardFault 过，见提交 8b39f37）。
 *
 *  这段 #if 一定要有：本套件按板级头分层的包含路径分板编译（-I $(BOARD_DIR)/<层>/Inc），少了它
 *  `make test BOARD=<直烧板>` 会红一片，而被当成"测试挂了"而不是"这套不适用"。
 * ================================================================ */

#if !BOARD_HAS_IAP_RECORD

static void case_write_path_is_short_circuited(void)
{
    flash_setup();

    /* 记录区里先塞一份**看起来完全有效**的记录：这样若短路失效，
       update_net_cfg 会认为"有效但内容不同"进而真的去擦写 —— 能被下面数出来。 */
    app_flash_iap_sys_info_t good;
    memset(&good, 0, sizeof(good));
    good.magic      = APP_FLASH_IAP_MAGIC;
    good.update_sta = APP_FLASH_IAP_UPDATED;
    good.config_crc = _iap_cfg_crc(&good);
    memcpy((void *)g_config, &good, sizeof(good));

    pl_flash_stub_reset();

    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);

    CHECK_MSG(pl_flash_stub_erase_count() == 0, "本板无记录区，却擦了 %d 次",
              pl_flash_stub_erase_count());
    CHECK_MSG(pl_flash_stub_program_count() == 0, "本板无记录区，却编程了 %d 次",
              pl_flash_stub_program_count());

    /* 记录内容必须原封不动 —— 短路是"什么都不做"，不是"写一份新的" */
    CHECK_MSG(memcmp((void *)g_config, &good, sizeof(good)) == 0, "短路径居然改动了记录区内容");

    /* 两个公开入口同样要报错而不是假装成功 */
    CHECK(0 != app_flash_iap_erase_config());
    CHECK(0 != app_flash_iap_write_config(&good));
    CHECK_MSG(pl_flash_stub_erase_count() == 0 && pl_flash_stub_program_count() == 0,
              "公开入口绕过了短路");
}

int main(void)
{
    printf("\n\033[33m⚠ 本板 BOARD_HAS_IAP_RECORD = 0（直烧板），"
           "记录读写用例不适用，只跑短路守卫\033[0m\n\n");

    int before = g_failures;
    printf("▶ 本板无记录区：擦/写一律不碰 Flash\n");
    case_write_path_is_short_circuited();
    printf("  %s（本用例失败 %d）\n", g_failures == before ? "通过" : "**失败**",
           g_failures - before);

    printf("\n用例 1 个，失败 %d 个\n", g_failures);
    return g_failures ? 1 : 0;
}

#else /* BOARD_HAS_IAP_RECORD */

/* ================================================================
 *  ① 空记录 → 播种有效骨架
 * ================================================================ */

static void case_empty_is_seeded(void)
{
    flash_setup();

    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);

    CHECK(REC->magic == APP_FLASH_IAP_MAGIC);
    CHECK(REC->update_sta == APP_FLASH_IAP_UPDATED);
    CHECK(app_flash_iap_is_config_valid(REC));
    CHECK(memcmp(REC->net_cfg.ip, IP_A, 4) == 0);
    CHECK(REC->net_cfg.port == PORT);
    CHECK(pl_flash_stub_erase_count() == 1);
}

/* ================================================================
 *  ② 编程中途掉电 → 损坏 → 下次写入自动修复   ← 核心用例
 *
 *  反向验证：把 update_net_cfg 的损坏分支改回 `return`，本用例立刻变红
 *  （第二次写入会静默跳过，记录永远停在损坏态）。
 * ================================================================ */

static void case_power_loss_leaves_corrupt_and_is_repaired(void)
{
    flash_setup();

    /* 先有一条有效记录 */
    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);
    CHECK(app_flash_iap_is_config_valid(REC));

    /* 再改一次 IP，但擦除完成后只写进第 0 个 word 就掉电 */
    pl_flash_stub_fail_after(1);
    app_flash_iap_update_net_cfg(IP_B, MASK, GW, PORT);

    /* 中间态必须真的是"既非空也非有效" —— 否则这个测试没测到点子上 */
    CHECK_MSG(!app_flash_iap_is_config_empty(REC), "掉电后记录被当成了空记录，本用例失去意义");
    CHECK_MSG(!app_flash_iap_is_config_valid(REC), "掉电后记录居然还是有效的？");
    printf("       掉电中间态：magic=0x%08X config_crc=0x%08X（既非空也非有效）\n",
           (unsigned)REC->magic, (unsigned)REC->config_crc);

    /* 恢复供电，上位机重下发 / 上电对账再写一次 */
    pl_flash_stub_fail_after(-1);
    app_flash_iap_update_net_cfg(IP_B, MASK, GW, PORT);

    CHECK(app_flash_iap_is_config_valid(REC));
    CHECK(memcmp(REC->net_cfg.ip, IP_B, 4) == 0);
    CHECK(REC->magic == APP_FLASH_IAP_MAGIC);
    CHECK(REC->update_sta == APP_FLASH_IAP_UPDATED);
}

/* ================================================================
 *  ③ 内容未变 → 不擦写（内部 Flash 擦除会硬停总线）
 * ================================================================ */

static void case_unchanged_skips_erase(void)
{
    flash_setup();

    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);
    int e1 = pl_flash_stub_erase_count();
    int p1 = pl_flash_stub_program_count();

    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT); /* 一模一样 */

    CHECK(pl_flash_stub_erase_count() == e1);
    CHECK(pl_flash_stub_program_count() == p1);

    /* 但换一个值就必须写 */
    app_flash_iap_update_net_cfg(IP_B, MASK, GW, PORT);
    CHECK(pl_flash_stub_erase_count() == e1 + 1);
}

/* ================================================================
 *  ④ 损坏记录不被去重短路
 *
 *  若把去重判定写在损坏判定之前，net_cfg 恰好一致时就会直接返回 ——
 *  记录永远修不好，且没有任何报错。
 * ================================================================ */

static void case_corrupt_is_not_deduped(void)
{
    flash_setup();

    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);
    CHECK(app_flash_iap_is_config_valid(REC));

    /* 只把 magic 抹掉模拟损坏，net_cfg 保持原样 */
    REC->magic = 0xFFFFFFFF;
    CHECK(!app_flash_iap_is_config_valid(REC));

    int e1 = pl_flash_stub_erase_count();

    /* 传入的 net_cfg 与记录里现有的完全一致 */
    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);

    CHECK_MSG(pl_flash_stub_erase_count() == e1 + 1, "损坏记录被去重短路了，没有重写");
    CHECK(app_flash_iap_is_config_valid(REC));
    CHECK(REC->magic == APP_FLASH_IAP_MAGIC);
}

/* ================================================================
 *  ⑤ CRC32 覆盖范围
 *
 *  _iap_cfg_crc 的覆盖范围（整条记录除去 config_crc 自身）原先在本文件里
 *  写了三遍，收成一处时最容易犯的错就是范围写窄。改动任何一个被覆盖的字段
 *  都必须让校验失败。
 * ================================================================ */

#define CHECK_FIELD_COVERED(name, stmt)                                                            \
    do {                                                                                           \
        flash_setup();                                                                             \
        app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);                                        \
        CHECK(app_flash_iap_is_config_valid(REC));                                                 \
        stmt;                                                                                      \
        CHECK_MSG(!app_flash_iap_is_config_valid(REC),                                             \
                  "改动 %s 后校验仍通过 —— CRC 覆盖范围漏了该字段", name);                         \
    } while (0)

static void case_crc_covers_every_field(void)
{
    CHECK_FIELD_COVERED("update_sta",    REC->update_sta = APP_FLASH_IAP_FAILED);
    CHECK_FIELD_COVERED("app_info.size", REC->app_info.size ^= 1U);
    CHECK_FIELD_COVERED("app_info.crc32", REC->app_info.crc32 ^= 1U);
    CHECK_FIELD_COVERED("app_info.version", REC->app_info.version[0] ^= 1U);
    CHECK_FIELD_COVERED("net_cfg.ip",    REC->net_cfg.ip[0] ^= 1U);
    CHECK_FIELD_COVERED("net_cfg.mask",  REC->net_cfg.mask[0] ^= 1U);
    CHECK_FIELD_COVERED("net_cfg.gw",    REC->net_cfg.gw[0] ^= 1U);
    CHECK_FIELD_COVERED("net_cfg.port",  REC->net_cfg.port ^= 1U);
}

/* ================================================================
 *  ⑥ 并发写不出现"擦除打断未完成的编程序列"
 *
 *  反向验证：去掉 app_iap_cfg.c 里的 s_lock，本用例应立刻变红 ——
 *  桩每编程一个 word 就让出一次 CPU，两个线程必然交错。
 * ================================================================ */

#define CONCURRENT_WRITES 40

static pthread_barrier_t g_barrier;

static void *writer_thread(void *arg)
{
    const uint8_t *ip = (const uint8_t *)arg;
    pthread_barrier_wait(&g_barrier);
    for (int i = 0; i < CONCURRENT_WRITES; i++)
        app_flash_iap_update_net_cfg(ip, MASK, GW, PORT);
    return NULL;
}

static void case_concurrent_writes_are_serialized(void)
{
    flash_setup();
    pl_flash_stub_set_yield_every(1); /* 每个 word 都让出 → 不加锁必交错 */

    pthread_barrier_init(&g_barrier, NULL, 3);
    pthread_t ta, tb;
    pthread_create(&ta, NULL, writer_thread, (void *)IP_A);
    pthread_create(&tb, NULL, writer_thread, (void *)IP_B);

    pthread_barrier_wait(&g_barrier); /* 两个线程同时起跑 */
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    pthread_barrier_destroy(&g_barrier);

    CHECK_MSG(!pl_flash_stub_interleaved(),
              "观察到 %d 次擦除、%d 次编程：有擦除打断未完成的编程序列",
              pl_flash_stub_erase_count(), pl_flash_stub_program_count());
    CHECK(app_flash_iap_is_config_valid(REC));
    CHECK(pl_flash_stub_erase_count() >= 1);
}


/* ================================================================
 *  ⑦ magic 检查：CRC 自洽但 magic 不是本协议 —— 必须判为无效
 *
 *  变异测试暴露的缺口：把 is_config_valid 的 magic 检查整个删掉，全套用例仍全绿
 *  （CRC 的覆盖范围包含 magic，"CRC 对"近乎蕴含"magic 对"）。
 *  但 magic 检查不是为了兜 CRC —— 它是为了挡住**别的格式写进来的记录**：
 *  布局碰巧自洽、CRC 也算得对，只是不属于本协议。这里显式构造一条。
 * ================================================================ */

static void case_foreign_magic_is_rejected(void)
{
    flash_setup();
    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);

    /* 改成"别的格式"的 magic，并把 CRC 重算成自洽的 */
    REC->magic       = 0xDEADBEEF;
    REC->config_crc  = _iap_cfg_crc(REC); /* 测试 include 了实现 TU，可用 static 函数 */

    CHECK_MSG(!app_flash_iap_is_config_valid(REC), "CRC 自洽但 magic 不对，仍被判为有效");
}

/* ================================================================
 *  ⑧ 写失败必须上报（dev_flash_int._write 的返回值传递）
 *
 *  变异测试暴露的缺口：把 _write 里 `r = -1` 去掉（回到"丢弃编程返回值"的旧写法），
 *  全套用例仍全绿。而调用方正是靠这个返回值判定"配置没落盘"。
 * ================================================================ */

static void case_write_failure_is_reported(void)
{
    flash_setup();

    pl_flash_stub_fail_after(0); /* 从下一次编程起全部失败 */
    app_flash_iap_sys_info_t info;
    memset(&info, 0, sizeof info);
    info.magic = APP_FLASH_IAP_MAGIC;

    int32_t r = app_flash_iap_write_config(&info);
    CHECK_MSG(r != 0, "编程全部失败，写接口却报了成功（返回 %d）", (int)r);

    pl_flash_stub_fail_after(-1);
    CHECK_MSG(app_flash_iap_write_config(&info) == 0, "正常情况下的写应该返回 0");
}

/* ================================================================
 *  ⑨ dev_storage_ops 的"0 = 成功"约定
 *
 *  变异测试暴露的缺口：把 _read 的 `return 0` 改成 `return len`（返回字节数），
 *  全套用例仍全绿。约定不钉住的话，下次有人按"返回字节数"理解就会出问题。
 * ================================================================ */

static void case_storage_returns_zero_on_success(void)
{
    flash_setup();
    app_flash_iap_update_net_cfg(IP_A, MASK, GW, PORT);

    dev_storage_t *st = app_flash_iap_get_storage();

    uint8_t buf[16];
    CHECK_MSG(dev_storage_read(st, 0, buf, sizeof buf) == 0, "读成功应返回 0（不返回字节数）");
    CHECK(memcmp(buf, REC, sizeof buf) == 0);

    /* 未擦除就写入"需要把 0 位写回 1"的内容 —— NOR 下必须失败。
       原样写回则不算失败：不需要改变任何位，是合法的空操作。 */
    buf[0] = 0xFF;
    CHECK_MSG(dev_storage_write(st, 0, buf, sizeof buf) != 0,
              "NOR 上把 0 位写回 1 应该失败，写接口却报了成功");
}

/* ================================================================ */

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"空记录 → 播种有效骨架", case_empty_is_seeded},
        {"编程中途掉电 → 损坏 → 自动修复", case_power_loss_leaves_corrupt_and_is_repaired},
        {"内容未变 → 不擦写", case_unchanged_skips_erase},
        {"损坏记录不被去重短路", case_corrupt_is_not_deduped},
        {"CRC32 覆盖全部字段", case_crc_covers_every_field},
        {"并发写不交错", case_concurrent_writes_are_serialized},
        {"异格式 magic（CRC 自洽）被拒", case_foreign_magic_is_rejected},
        {"写失败必须上报", case_write_failure_is_reported},
        {"存储接口 0 = 成功的约定", case_storage_returns_zero_on_success},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = g_failures;
        printf("\n▶ %s\n", cases[i].name);
        cases[i].fn();
        printf("  %s（本用例失败 %d）\n", g_failures == before ? "通过" : "**失败**",
               g_failures - before);
    }

    printf("\n用例 %zu 个，失败 %d 个\n", sizeof(cases) / sizeof(cases[0]), g_failures);
    return g_failures ? 1 : 0;
}

#endif /* BOARD_HAS_IAP_RECORD */
