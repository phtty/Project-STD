/**
 * @file    test_cfg_sched.c
 * @brief   配置调度器 host 单测 —— 记录归属、块位扫描、容量门槛、持久化往返
 *
 * 被测代码是生产源码本体（Application/Src/app_cfg_sched.c + Device/Storage/cfg_record.c），
 * 不做任何替换。存储侧用一块 RAM 假 Flash + dev_w25qxx_get() 桩，按 NOR 语义实现
 * （写 = 按位与，目标区间非 0xFF 则先擦整扇区），与 dev_w25qxx._write 的 RMW 同构 ——
 * 否则测不出真实的位翻转约束与扇区擦除影响。
 *
 * 移植来源：参考工程 Project_STD_B/test/test_cfg_sched.c。三处按本工程改动：
 *   · CFG_RECORD_MAX_IMAGE 2048 → 2600（本工程最大载荷是 P10 320×64 显存 2565B）
 *   · s_ready 门槛改成 cap >= FONT_LIB_TOTAL_BYTES + CFG_REGION_BYTES
 *     → 假 Flash 容量必须 32MB 级；参考工程那版用 64KB，在这里会被判为"不可用"
 *   · FONT_LIB_TOTAL_BYTES 30,713,088（两工程同值）
 *
 * 相对参考工程补强的三处：
 *   1. 参考工程 case_foreign_record_ignored 是**假信心用例**（它自己在 commit 114383b
 *      的提交信息里承认"没有建立'陌生记录受保护'这条性质"）：陌生记录放在块 6，
 *      而落位目标恰好是块 0，两者从未相撞，通过与否与归属校验无关。
 *      这里的 case_foreign_record 把陌生记录直接放在被测所有者会去认领的块上，
 *      断言的是**真性质**（名字不符者绝不被读成有效配置）+ 如实记录已知取舍
 *      （撞上落位目标的陌生记录会被覆盖）。
 *   2. 参考工程所有用例都在同一进程内，块位一旦落位就不再重算，而真实风险恰恰是
 *      "重启后重扫能否找回自己写的记录"。这里用 fork() 模拟重启（假 Flash 放在
 *      MAP_SHARED 映射上，跨进程可见），补上 save → 重启 → load 的端到端用例，
 *      且刻意让记录落在"与其注册序号不符"的块上，使重扫成为必经路径。
 *   3. 补上参考工程缺失的覆盖：未就绪路径（load = IO_ERR / save = -1）、载荷超限
 *      可回报、注册规则、加载遍顺序与重扫、载荷 CRC 破坏。
 *
 * 本工程相对参考工程有三处修正，各有用例守着：
 *   · 容量门槛与编译期契约同源（case_capacity_gate_font_contract）：
 *     门槛必须是 cap >= FONT_LIB_TOTAL_BYTES + CFG_REGION_BYTES，而不是"装得下配置区"。
 *     退回旧写法时，JEDEC 被误读成 8MB 会让块地址落进字库区，首次 save 就把字库擦了
 *     —— 该用例正是用一条"字库哨兵字节"把这个后果测出来的。
 *   · 绑定不在失败时闩锁（case_bind_recovers_when_capacity_appears）：容量 0 时
 *     _cfg_sched_bind 不置 s_bound，器件随后可用时能恢复；旧写法一旦在容量 0 时闩死，
 *     本上电周期内持久化永久禁用且无重试机会。
 *   · LDI 的幂等加载不缓存 IO_ERR（case_ldi_cfg_retries_after_io_err）：
 *     首次读失败后不置 s_load_done，条件变好时再调用能读到。
 *
 * 隔离方式：调度器状态（所有者表、块位表、绑定/就绪标志）是文件级静态且无重置接口，
 * 所以每个用例 fork 出独立进程；进程内再 fork 出"重启后"的新进程。
 */

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app_cfg_sched.h"
#include "app_render.h" /* FONT_LIB_TOTAL_BYTES / RENDER_PERSIST_PAYLOAD_MAX */
#include "cfg_record.h"

/* LDI 配置模块的实现 TU 直接包含进来（本文件因此**不能**再把 app_ldi_cfg.c
   列入编译源，否则符号重复）。原因：它的注册入口 _app_flash_ldi_cfg_register
   是 static、由 sw_dev_initcall 调用，host 上没有 initcall 段运行器（Kernel/Src/
   initcall.c 依赖链接脚本给出的 __hw/sw_initcall_start/_end 边界符号），
   从外部无法触达。包含 .c 让测试能真正跑到它，而不是为了可测性去改生产代码。 */
#include "../Application/Src/LDI/app_ldi_cfg.c"

/* ================================================================
 *  断言与统计
 * ================================================================ */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            printf("      \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                       \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                   \
            printf("\n");                                                          \
        }                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  假 Flash（RAM）—— 按 NOR 语义
 * ================================================================ */

/* 容量必须过本工程的门槛：cap >= FONT_LIB_TOTAL_BYTES + CFG_REGION_BYTES ≈ 29.33MB。
   取 32MB（= CFG_CAP_CONTRACT，W25Q256），块地址落在数组尾部。 */
#define FAKE_CAP (32U * 1024U * 1024U)

/* MAP_SHARED：fork 出的"重启进程"必须能看到上一次上电写的字节。普通全局数组做不到
   —— 子进程的写入只留在自己的 COW 页里，父进程与兄弟进程都看不到。 */
static uint8_t      *s_flash;
static dev_storage_t s_dev;
static uint32_t      s_cap; /* 当前上报容量：用例可在两次调用之间改（模拟识别变化/误读） */

/* 器件访问计数：用来断言"该在碰器件之前就返回"的路径确实没碰 */
static uint32_t s_reads;
static uint32_t s_writes;

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
    if ((uint64_t)addr + len > FAKE_CAP) return -1;

    /* 与 dev_w25qxx._write 同构的 RMW：读回整扇区 → 目标区间非 0xFF 则擦除 →
       合并 → 整扇区回写。回写用"按位与"模拟 NOR 只能把 1 写成 0 的物理约束。 */
    static uint8_t sec[CFG_REGION_SECTOR];

    while (len) {
        uint32_t sa    = addr & ~(CFG_REGION_SECTOR - 1U);
        uint32_t off   = addr - sa;
        uint32_t chunk = CFG_REGION_SECTOR - off;
        if (chunk > len) chunk = len;

        memcpy(sec, s_flash + sa, CFG_REGION_SECTOR);

        bool need = false;
        for (uint32_t i = off; i < off + chunk; i++)
            if (s_flash[sa + i] != 0xFFU) {
                need = true;
                break;
            }

        if (need) {
            /* 擦除必须发生在**器件侧**（真驱动发的是 SECTOR_ERASE 命令）；
               只把本地 sec 置 0xFF 是不够的 —— 下面的"按位与"会把新内容与旧内容
               与在一起，落盘的是两者混合。 */
            memset(s_flash + sa, 0xFF, CFG_REGION_SECTOR);
            memset(sec, 0xFF, CFG_REGION_SECTOR);
        }
        memcpy(&sec[off], buf, chunk);

        for (uint32_t i = 0; i < CFG_REGION_SECTOR; i++)
            s_flash[sa + i] &= sec[i];

        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

static uint32_t fake_capacity(dev_storage_t *d)
{
    (void)d;
    return s_cap;
}

static const dev_storage_ops_t fake_ops = {
    .read     = fake_read,
    .write    = fake_write,
    .capacity = fake_capacity,
};

/** @brief dev_w25qxx_get 的桩：调度器据此拿到假 Flash */
dev_storage_t *dev_w25qxx_get(void)
{
    return &s_dev;
}

/* ================================================================
 *  装置
 * ================================================================ */

static uint32_t block_addr(uint8_t blk)
{
    return s_cap - (uint32_t)(blk + 1) * CFG_REGION_SECTOR;
}

/** @brief 绕过调度器，直接往指定块写一条记录 —— 模拟"上一次固件留下的数据" */
static void seed_record(uint8_t blk, const char *name, uint16_t ver, const char *payload)
{
    static uint8_t scratch[CFG_RECORD_HDR_SIZE + 64];
    int32_t        r = cfg_record_save(&s_dev, block_addr(blk), name, ver, NULL,
                                       (const uint8_t *)payload, (uint16_t)strlen(payload),
                                       scratch, sizeof(scratch));
    if (r != 0) {
        printf("      seed 失败 (blk=%u)\n", blk);
        g_fail++;
    }
}

/** @brief 读某块的记录头（黑盒观察块位：不看调度器的内部数组）*/
static bool read_hdr(uint8_t blk, cfg_record_hdr_t *hdr)
{
    return fake_read(&s_dev, block_addr(blk), (uint8_t *)hdr, sizeof(*hdr)) == 0;
}

static const cfg_sched_desc_t desc_a = {.name = "mod_a", .version = 1, .load = NULL};
static const cfg_sched_desc_t desc_b = {.name = "mod_b", .version = 1, .load = NULL};

/** @brief 在子进程里跑 fn（用于模拟"重启"），返回它是否成功；定义在文件末尾 */
static bool run_in_child(void (*fn)(void));

static void fake_reset(void)
{
    memset(s_flash, 0xFF, FAKE_CAP); /* 擦除态全 0xFF */
    s_dev.ops      = &fake_ops;
    s_dev.capacity = FAKE_CAP;
    s_cap          = FAKE_CAP;
    s_reads        = 0;
    s_writes       = 0;
}

/* ================================================================
 *  用例：基本读写
 * ================================================================ */

static void case_empty_flash(void)
{
    TEST_BEGIN("空 Flash：EMPTY → 首次落位 → 写入 → 读回 → 去重跳过");

    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);
    CHECK(app_cfg_sched_ready());

    uint8_t  buf[64] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_EMPTY);

    CHECK(app_cfg_sched_save(id, (const uint8_t *)"hello", 5) == 0);
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 5 && memcmp(buf, "hello", 5) == 0);

    /* 首次上电（无记录）时落位在"注册序号那块" = 块 0 */
    cfg_record_hdr_t hdr;
    CHECK(read_hdr(0, &hdr));
    CHECK(strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0);
    CHECK(hdr.version == 1 && hdr.len == 5);

    /* 不落位的块保持擦除态（不占块） */
    CHECK(read_hdr(1, &hdr));
    CHECK((uint8_t)hdr.name[0] == 0xFFU);

    /* 同一内容重复保存：读回比对一致 → 跳过写（NOR 擦写磨损保护） */
    uint32_t writes_before = s_writes;
    CHECK(app_cfg_sched_save(id, (const uint8_t *)"hello", 5) == 0);
    CHECK_MSG(s_writes == writes_before, "重复保存同一内容仍写了 Flash（去重失效）");

    /* 内容变了则必须真的落盘 */
    CHECK(app_cfg_sched_save(id, (const uint8_t *)"world", 5) == 0);
    CHECK(s_writes > writes_before);
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 5 && memcmp(buf, "world", 5) == 0);
}

static void case_order_independent(void)
{
    TEST_BEGIN("记录位置与注册顺序无关（扫描认领的核心）");

    /* 模拟"上一次固件"把记录写在了与其注册序号不相符的块上：
       mod_a 排在注册序 0 却存在块 5，mod_b 排在序 1 却存在块 2。
       按"块 = 注册序号"去找必然落空（块 0/1）。 */
    seed_record(5, "mod_a", 1, "AAA");
    seed_record(2, "mod_b", 1, "BBB");

    uint8_t ia = app_cfg_sched_register(&desc_a);
    uint8_t ib = app_cfg_sched_register(&desc_b);
    CHECK(ia != 0xFF && ib != 0xFF && ia != ib);

    uint8_t  buf[64] = {0};
    uint16_t len     = 0;

    CHECK_MSG(app_cfg_sched_load(ia, buf, sizeof(buf), &len) == CFG_REC_OK,
              "mod_a 未被找到（块位仍依赖注册序号）");
    CHECK(len == 3 && memcmp(buf, "AAA", 3) == 0);

    memset(buf, 0, sizeof(buf));
    CHECK_MSG(app_cfg_sched_load(ib, buf, sizeof(buf), &len) == CFG_REC_OK,
              "mod_b 未被找到（块位仍依赖注册序号）");
    CHECK(len == 3 && memcmp(buf, "BBB", 3) == 0);

    /* 认领后原地保存：不搬家 */
    CHECK(app_cfg_sched_save(ia, (const uint8_t *)"AAA2", 4) == 0);
    cfg_record_hdr_t hdr;
    CHECK(read_hdr(5, &hdr) && strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0);
    CHECK(hdr.len == 4);
}

static void case_version_bump_keeps_address(void)
{
    TEST_BEGIN("版本升级：判定失效但不搬家（回落默认而非重写）");

    /* 记录以 version=1 写入，注册方声明 version=2 */
    seed_record(3, "mod_a", 1, "OLD");
    static const cfg_sched_desc_t desc_v2 = {.name = "mod_a", .version = 2, .load = NULL};
    uint8_t id = app_cfg_sched_register(&desc_v2);
    CHECK(id != 0xFF);

    uint8_t  buf[64] = {0};
    uint16_t len     = 0;
    CHECK_MSG(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_INVALID,
              "版本不符应判失效（而非 EMPTY —— 那说明扫描没找到它）");

    /* 关键：记录仍在块 3，没有被搬到"注册序号"对应的块上，也没被改写 */
    cfg_record_hdr_t hdr;
    CHECK(fake_read(&s_dev, block_addr(3), (uint8_t *)&hdr, sizeof(hdr)) == 0);
    CHECK(strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0);
    CHECK(hdr.version == 1);
}

static void case_crc_corruption(void)
{
    TEST_BEGIN("载荷被破坏：CRC 不符判失效，绝不当作有效配置");

    seed_record(4, "mod_a", 1, "PAYLOAD");
    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);

    /* 破坏载荷首字节（只动 Flash，不动记录头）*/
    s_flash[block_addr(4) + CFG_RECORD_HDR_SIZE] ^= 0x01U;

    uint8_t  buf[64] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_INVALID);
}

/* ================================================================
 *  用例：陌生记录（参考工程那条假信心用例的重写版）
 * ================================================================ */

static void case_foreign_record(void)
{
    TEST_BEGIN("陌生记录：不被误读为配置；撞上落位目标则被覆盖（已知取舍）");

    /* 陌生记录刻意做成"看起来完全合法"的样子：版本相同、长度相同、载荷内容
       与被测所有者稍后要写的一模一样。于是"load 返回 OK"只可能来自归属校验
       被跳过 —— 用例因此真的会红，而不是靠数据碰巧对不上。
       （反过来，断言"内容对不对"是没有意义的：内容本来就一样。） */
    seed_record(0, "someone_else", 1, "SAME");

    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);

    uint8_t  buf[64];
    uint16_t len = 0;
    memset(buf, 0x5A, sizeof(buf));

    cfg_rec_sta_t sta = app_cfg_sched_load(id, buf, sizeof(buf), &len);
    CHECK_MSG(sta != CFG_REC_OK, "把别人的记录当成了自己的配置（归属校验失效）");
    CHECK_MSG(sta == CFG_REC_INVALID, "名字不符应判 INVALID（EMPTY 说明扫描压根没看这块）");

    /* 已知取舍（见 app_cfg_sched.c 的 _cfg_sched_scan 注释）：持有陌生记录的块
       不进 taken[]，需要落位的所有者会把它当"空块"认领，首次 save 直接覆盖。
       这里如实断言覆盖行为。若哪天改成"保护陌生记录"，这条会红 —— 改的人必须
       同时回答"8 个块都被陌生记录占满时，新模块去哪"，那是有意为之的取舍，
       不该被悄悄改掉。 */
    CHECK(app_cfg_sched_save(id, (const uint8_t *)"SAME", 4) == 0);

    cfg_record_hdr_t hdr;
    CHECK(read_hdr(0, &hdr));
    CHECK_MSG(strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0,
              "落位没有覆盖陌生记录（行为已变：请同步 app_cfg_sched.c 的取舍说明）");
    CHECK(hdr.version == 1 && hdr.len == 4);

    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 4 && memcmp(buf, "SAME", 4) == 0);
}

/* ================================================================
 *  用例：save → 重启 → load（参考工程缺的端到端）
 *
 *  "重启" = 从一个**没碰过调度器**的进程再 fork 一个子进程：它的静态状态全新
 *  （所有者表空、块位表全 0xFF、绑定标志清零），而假 Flash 在共享映射上，
 *  上一次上电写下的字节对它可见。这正好复现真实风险：块位不是"落位时记住的"，
 *  必须靠重扫按名字找回来。
 * ================================================================ */

static void reboot_phase1_write(void)
{
    /* 上一次固件留在块 0 的记录：名字属于本次仍会注册的 mod_b */
    seed_record(0, "mod_b", 1, "B0");

    uint8_t ia = app_cfg_sched_register(&desc_a);
    uint8_t ib = app_cfg_sched_register(&desc_b);
    CHECK(ia == 0 && ib == 1);

    /* 块 0 被 mod_b 认领 → mod_a 只能落位到下一个空块（块 1）：本次 save 因此
       写在"与其注册序号不符"的块上，重启后必须按名字才找得回来。 */
    CHECK(app_cfg_sched_save(ia, (const uint8_t *)"A1", 2) == 0);
    CHECK(app_cfg_sched_save(ib, (const uint8_t *)"B1", 2) == 0);

    cfg_record_hdr_t hdr;
    CHECK(read_hdr(1, &hdr) && strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0);
    CHECK(read_hdr(0, &hdr) && strncmp(hdr.name, "mod_b", CFG_RECORD_NAME_MAX) == 0);

    uint8_t  buf[16] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(ia, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 2 && memcmp(buf, "A1", 2) == 0);
}

static void reboot_phase2_read(void)
{
    /* 重启后：静态状态全新，注册顺序与上电时一致（initcall 顺序由构建决定） */
    uint8_t ia = app_cfg_sched_register(&desc_a);
    uint8_t ib = app_cfg_sched_register(&desc_b);
    CHECK(ia == 0 && ib == 1);

    uint8_t  buf[16] = {0};
    uint16_t len     = 0;

    cfg_rec_sta_t sa = app_cfg_sched_load(ia, buf, sizeof(buf), &len);
    CHECK_MSG(sa == CFG_REC_OK,
              "重启后没找回自己写的记录（若为 INVALID：找的是块 0，那是 mod_b 的记录）");
    CHECK(len == 2 && memcmp(buf, "A1", 2) == 0);

    memset(buf, 0, sizeof(buf));
    CHECK(app_cfg_sched_load(ib, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 2 && memcmp(buf, "B1", 2) == 0);
}

static void case_save_reboot_load(void)
{
    TEST_BEGIN("save → 重启 → load：重扫按名字找回，块位不依赖注册序号");

    CHECK_MSG(run_in_child(reboot_phase1_write), "阶段一（写）失败");
    CHECK_MSG(run_in_child(reboot_phase2_read), "阶段二（重启后读）失败");
}

/* ================================================================
 *  用例：容量门槛（本工程相对参考工程的第一处偏离）
 * ================================================================ */

static void case_capacity_gate_font_contract(void)
{
    TEST_BEGIN("容量门槛与字库契约同源：8MB 判为未就绪，且一个字节都不碰");

    /* JEDEC ID 被识别成一个"合法但更小"的值（0x17 = 8MB）。参考工程的门槛只要求
       cap >= CFG_REGION_BYTES(32KB)，于是 8MB 会通过 —— 而块 0 = cap - 4096 落在
       字库区（0 ~ FONT_LIB_TOTAL_BYTES）内部，首次 save 的扇区擦除直接毁掉字库。
       下面这条断言把这个前提写死：门槛必须与编译期契约同源。 */
    const uint32_t cap           = 8U * 1024U * 1024U;
    const uint32_t would_be_blk0 = cap - CFG_REGION_SECTOR;
    s_cap                        = cap;
    CHECK_MSG(would_be_blk0 < FONT_LIB_TOTAL_BYTES,
              "前提变了（8MB 的块 0 不在字库区内），本例的论证需重写");

    s_flash[would_be_blk0] = 0x5A; /* 假装这是字库数据 */

    CHECK(!app_cfg_sched_ready());

    /* 注册与容量无关：器件不可用只让配置回落默认值，不影响模块启动 */
    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);

    uint8_t  buf[16] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_IO_ERR);
    CHECK_MSG(app_cfg_sched_save(id, (const uint8_t *)"x", 1) == -1,
              "未就绪时 save 必须可回报地失败");

    /* 未就绪必须在**碰器件之前**返回 */
    CHECK_MSG(s_reads == 0 && s_writes == 0, "未就绪时仍访问了器件（读 %u / 写 %u）",
              (unsigned)s_reads, (unsigned)s_writes);
    CHECK_MSG(s_flash[would_be_blk0] == 0x5A, "字库区被擦写");

    /* 越界句柄：安全失败，不索引到别人头上 */
    CHECK(app_cfg_sched_load(0xFF, buf, sizeof(buf), &len) == CFG_REC_IO_ERR);
    CHECK(app_cfg_sched_save(0xFF, (const uint8_t *)"x", 1) == -1);
    CHECK(app_cfg_sched_save(id, NULL, 0) == -1);
}

static void case_bind_recovers_when_capacity_appears(void)
{
    TEST_BEGIN("容量 0 时判定不闩锁：器件随后可用 → 持久化自行恢复");

    s_cap = 0; /* JEDEC ID 未识别：容量"未知"，不是"不可用" */

    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);

    CHECK(!app_cfg_sched_ready());

    uint8_t  buf[32] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_IO_ERR);
    CHECK(app_cfg_sched_save(id, (const uint8_t *)"early", 5) == -1);
    CHECK(s_reads == 0 && s_writes == 0);

    /* 器件被识别出来了（真实场景：SPI 首次读 ID 失败后重试成功，或第一次 load
       发生在 dev_w25qxx 就绪之前）。若在容量 0 时就把绑定标志闩死，本上电周期
       内持久化永久禁用且无重试机会 —— 配置"存了重启就不生效"。 */
    s_cap = FAKE_CAP;
    CHECK_MSG(app_cfg_sched_ready(), "容量 0 时被闩死：器件后来可用也救不回来");

    CHECK(app_cfg_sched_save(id, (const uint8_t *)"early", 5) == 0);
    CHECK(app_cfg_sched_load(id, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 5 && memcmp(buf, "early", 5) == 0);
}

/* ================================================================
 *  用例：载荷边界（够不够装 / 装不下要能回报）
 * ================================================================ */

static void case_payload_bounds(void)
{
    TEST_BEGIN("载荷边界：最大显存记录存得下；超限可回报且不破坏已有记录");

    uint8_t id = app_cfg_sched_register(&desc_a);
    CHECK(id != 0xFF);

    /* 本工程最大的真实载荷 = 渲染显存（P10 320×64 的 1bpp 位图 + 5B 头）。
       把它写通就是"CFG_RECORD_MAX_IMAGE 取值够用"的守门人：照抄参考工程的
       2048 时，这里会红（组包缓冲只有 24+2048，save 返回 -1）。 */
    static uint8_t big[RENDER_PERSIST_PAYLOAD_MAX];
    static uint8_t rd[CFG_RECORD_MAX_IMAGE];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 31U + 7U);

    CHECK_MSG(app_cfg_sched_save(id, big, (uint16_t)sizeof(big)) == 0,
              "最大显存记录（%u 字节）存不下去：CFG_RECORD_MAX_IMAGE 偏小",
              (unsigned)sizeof(big));

    uint16_t len = 0;
    CHECK(app_cfg_sched_load(id, rd, sizeof(rd), &len) == CFG_REC_OK);
    CHECK(len == sizeof(big) && memcmp(rd, big, sizeof(big)) == 0);

    /* 组包缓冲的硬上界：恰好 CFG_RECORD_MAX_IMAGE 可以 */
    static uint8_t max_img[CFG_RECORD_MAX_IMAGE];
    for (size_t i = 0; i < sizeof(max_img); i++)
        max_img[i] = (uint8_t)(0xFFU - (i & 0x3FU));

    CHECK(app_cfg_sched_save(id, max_img, (uint16_t)sizeof(max_img)) == 0);
    CHECK(app_cfg_sched_load(id, rd, sizeof(rd), &len) == CFG_REC_OK);
    CHECK(len == CFG_RECORD_MAX_IMAGE && memcmp(rd, max_img, sizeof(max_img)) == 0);

    /* 超出一个字节：必须可回报地失败（返回负值），而不是静默截断或越界 */
    uint32_t writes_before = s_writes;
    CHECK_MSG(app_cfg_sched_save(id, max_img, CFG_RECORD_MAX_IMAGE + 1U) < 0,
              "超限载荷没有可回报地失败");
    CHECK(s_writes == writes_before); /* 失败发生在动 Flash 之前 */

    /* uint16 回绕：payload_len 使 24 + payload_len 溢出 16 位时，容量校验必须仍然
       拦得住。此前 total 先在 uint16 里算，(65536 → 0) 会让校验放行，随后
       memcpy 把 64KB 写穿 s_scratch（2624B）—— ASan 下是 global-buffer-overflow。
       这条是修复后才加的：修复前它会红（正是它该有的样子）。 */
    static uint8_t huge[65535];
    uint32_t       writes_before_huge = s_writes;
    CHECK_MSG(app_cfg_sched_save(id, huge, 65535) < 0, "uint16 回绕绕过了容量校验");
    CHECK_MSG(app_cfg_sched_save(id, huge, 65512) < 0, "回绕边界（total==0）绕过了容量校验");
    CHECK(s_writes == writes_before_huge);

    /* 已有记录未被破坏 */
    CHECK(app_cfg_sched_load(id, rd, sizeof(rd), &len) == CFG_REC_OK);
    CHECK(len == CFG_RECORD_MAX_IMAGE && memcmp(rd, max_img, sizeof(max_img)) == 0);

    /* 调用方缓冲装不下记录长度 → INVALID，而不是写爆或截断 */
    CHECK(app_cfg_sched_load(id, rd, CFG_RECORD_MAX_IMAGE - 1U, &len) == CFG_REC_INVALID);
}

/* ================================================================
 *  用例：注册规则 / 加载遍 / 重扫
 * ================================================================ */

static void case_register_rules(void)
{
    TEST_BEGIN("注册规则：空名/超长名/重名/满员一律忽略（返回 0xFF）");

    CHECK(app_cfg_sched_register(NULL) == 0xFF);
    CHECK(app_cfg_sched_register(&(cfg_sched_desc_t){.name = "", .version = 1}) == 0xFF);
    /* 16 字符正好填满 name[16]，没有 NUL 位置 → 拒绝（否则头里与注册名比较不等价）*/
    CHECK(app_cfg_sched_register(&(cfg_sched_desc_t){.name = "0123456789abcdef", .version = 1}) ==
          0xFF);
    /* 15 字符是允许的上界 */
    static const cfg_sched_desc_t name15 = {.name = "0123456789abcde", .version = 1};
    CHECK(app_cfg_sched_register(&name15) == 0);

    /* 重名忽略，且不消耗块位 */
    static const cfg_sched_desc_t dup = {.name = "dup", .version = 1};
    CHECK(app_cfg_sched_register(&dup) == 1);
    CHECK_MSG(app_cfg_sched_register(&dup) == 0xFF, "同名重复注册应被忽略");

    /* 满员：id 2..7 依次分完，第 9 个注册者被拒 */
    static char names[CFG_REGION_MAX_BLOCKS][8];
    for (uint8_t i = 0; i < CFG_REGION_MAX_BLOCKS; i++) {
        snprintf(names[i], sizeof(names[i]), "n%u", (unsigned)i);
        cfg_sched_desc_t d   = {.name = names[i], .version = 1, .load = NULL};
        uint8_t          got = app_cfg_sched_register(&d);
        if (i < CFG_REGION_MAX_BLOCKS - 2U)
            CHECK(got == (uint8_t)(i + 2U));
        else
            CHECK_MSG(got == 0xFF, "满员后应拒绝新注册（i=%u）", (unsigned)i);
    }
}

static int  s_load_calls;
static char s_load_seq[CFG_REGION_MAX_BLOCKS];

static void cb_a(void)
{
    s_load_seq[s_load_calls++] = 'A';
}
static void cb_c(void)
{
    s_load_seq[s_load_calls++] = 'C';
}

static void case_load_all_and_rescan(void)
{
    TEST_BEGIN("加载遍按注册顺序调用（NULL 跳过）；新增注册者触发重扫后记录仍可找回");

    seed_record(0, "mod_b", 1, "B0"); /* 上一次固件留下的记录 */

    static const cfg_sched_desc_t da = {.name = "mod_a", .version = 1, .load = cb_a};
    static const cfg_sched_desc_t db = {.name = "mod_b", .version = 1, .load = NULL};
    static const cfg_sched_desc_t dc = {.name = "mod_c", .version = 1, .load = cb_c};

    uint8_t ia = app_cfg_sched_register(&da);
    uint8_t ib = app_cfg_sched_register(&db);
    CHECK(ia == 0 && ib == 1);

    /* mod_b 认领块 0：记录是上一次固件写的，名字与版本都对 → 直接有效 */
    uint8_t  buf[32] = {0};
    uint16_t len     = 0;
    CHECK(app_cfg_sched_load(ib, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 2 && memcmp(buf, "B0", 2) == 0);

    /* mod_a 于是落位到块 1（与其注册序号 0 不符）*/
    CHECK(app_cfg_sched_save(ia, (const uint8_t *)"A1", 2) == 0);
    cfg_record_hdr_t hdr;
    CHECK(read_hdr(1, &hdr) && strncmp(hdr.name, "mod_a", CFG_RECORD_NAME_MAX) == 0);

    /* 新增注册者 → 所有者数变化 → 下次使用触发整表重扫 */
    uint8_t ic = app_cfg_sched_register(&dc);
    CHECK(ic == 2);

    s_load_calls = 0;
    app_cfg_sched_load_all();
    CHECK(s_load_calls == 2);
    CHECK(s_load_seq[0] == 'A' && s_load_seq[1] == 'C'); /* db 的 load 为 NULL，跳过 */

    /* 重扫之后按名字找回原地址（块位表不是"记住的"，是"扫出来的"）*/
    CHECK(app_cfg_sched_load(ia, buf, sizeof(buf), &len) == CFG_REC_OK);
    CHECK(len == 2 && memcmp(buf, "A1", 2) == 0);
    CHECK(app_cfg_sched_load(ib, buf, sizeof(buf), &len) == CFG_REC_OK);
}

/* ================================================================
 *  用例：LDI 配置的幂等加载（本工程相对参考工程的第三处偏离）
 * ================================================================ */

static void case_ldi_cfg_retries_after_io_err(void)
{
    TEST_BEGIN("LDI 配置：首次读失败（IO_ERR）不缓存，条件变好后重试能读到");

    /* 器件未识别 */
    s_cap = 0;
    _app_flash_ldi_cfg_register();
    CHECK(s_cfg_id != 0xFF);

    app_flash_ldi_cfg_info_t info;
    memset(&info, 0xEE, sizeof(info));
    CHECK(!app_flash_ldi_load_config(&info));
    CHECK_MSG(!s_load_done,
              "IO_ERR 被当成了'读过了'（s_load_done 置位）：本上电周期内再无重试机会");

    /* 器件随后可用：容量到位（同时验证绑定不在容量 0 时闩锁）*/
    s_cap = FAKE_CAP;

    app_flash_ldi_cfg_info_t want;
    memset(&want, 0, sizeof(want));
    want.device_ip[0]  = 192;
    want.device_ip[3]  = 7;
    want.device_port   = 0x1F90;
    memcpy(want.lane_hex, "12345", 5);
    memcpy(want.cert, "CERT0001", 8);
    want.module_count  = 2;
    want.modules[0].device_type  = 0xE1;
    want.modules[0].device_index = 1;
    want.modules[1].device_type  = 0xE5;
    want.modules[1].device_index = 2;
    CHECK(app_flash_ldi_save_config(&want) == 0);

    app_flash_ldi_cfg_info_t got;
    memset(&got, 0, sizeof(got));
    CHECK_MSG(app_flash_ldi_load_config(&got), "条件变好后重试仍未读到（IO_ERR 被缓存了）");
    CHECK(s_load_done);
    CHECK(memcmp(&got, &want, sizeof(want)) == 0);

    /* 读到有效配置后再存一次：内容未变 → 去重跳过 */
    uint32_t writes_before = s_writes;
    CHECK(app_flash_ldi_save_config(&want) == 0);
    CHECK(s_writes == writes_before);
}

/* ================================================================
 *  入口
 *
 *  调度器状态是静态的、无重置接口，所以每个用例 fork 出独立进程 ——
 *  否则前一个用例注册的所有者与块位会污染后一个。
 * ================================================================ */

/** @brief 在子进程里跑 fn（模拟"重启后"），返回它是否成功 */
static bool run_in_child(void (*fn)(void))
{
    fflush(stdout); /* 避免 fork 后缓冲区被子进程重复输出 */
    pid_t pid = fork();
    if (pid == 0) {
        int before = g_fail;
        fn();
        fflush(stdout); /* _exit 不刷 stdio 缓冲：管道下整个子进程的输出会被丢掉 */
        _exit(g_fail == before ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

typedef void (*case_fn_t)(void);

static const struct {
    const char *name;
    case_fn_t   fn;
} s_cases[] = {
    {"空 Flash 首次落位",            case_empty_flash},
    {"记录位置与顺序无关",           case_order_independent},
    {"版本升级不搬家",               case_version_bump_keeps_address},
    {"载荷 CRC 破坏判失效",          case_crc_corruption},
    {"陌生记录不误读 / 撞上则覆盖",  case_foreign_record},
    {"save → 重启 → load",           case_save_reboot_load},
    {"容量门槛同源于字库契约",       case_capacity_gate_font_contract},
    {"容量 0 不闩锁、随后恢复",      case_bind_recovers_when_capacity_appears},
    {"载荷边界与超限回报",           case_payload_bounds},
    {"注册规则",                     case_register_rules},
    {"加载遍顺序与触发重扫",         case_load_all_and_rescan},
    {"LDI 配置 IO_ERR 不缓存",       case_ldi_cfg_retries_after_io_err},
};

int main(void)
{
    /* 假 Flash 放共享映射：fork 出的"重启进程"要能看到上一次上电写的字节 */
    s_flash = mmap(NULL, FAKE_CAP, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s_flash == MAP_FAILED) {
        printf("假 Flash 映射失败\n");
        return 1;
    }
    fake_reset();

    printf("配置调度器 host 单测\n");

    const size_t n_cases = sizeof(s_cases) / sizeof(s_cases[0]);
    int          failed  = 0;

    for (size_t i = 0; i < n_cases; i++) {
        fflush(stdout); /* 避免 fork 后缓冲区被子进程重复输出 */

        pid_t pid = fork();
        if (pid == 0) {
            g_pass = g_fail = 0; /* 子进程内独立计数 */
            fake_reset();
            s_cases[i].fn();
            printf("    → 通过 %d，失败 %d\n", g_pass, g_fail);
            fflush(stdout); /* _exit 不刷 stdio 缓冲 */
            _exit(g_fail == 0 ? 0 : 1);
        }

        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            failed++;
            if (WIFSIGNALED(st))
                printf("  \033[31m✘\033[0m %s：子进程被信号 %d 终止\n", s_cases[i].name,
                       WTERMSIG(st));
            else
                printf("  \033[31m✘\033[0m %s\n", s_cases[i].name);
        }
    }

    printf("\n用例 %zu 个，失败 %d 个\n", n_cases, failed);
    return failed == 0 ? 0 : 1;
}
