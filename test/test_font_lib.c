/**
 * @file    test_font_lib.c
 * @brief   板级字库表：累加偏移必须逐条等于实物映像的基址
 *
 * **为什么需要这个测试**：字库单元在 W25Qxx 上线性连续排列，某项的偏移 = 前面所有
 * 项的 unit_size 之和 —— 于是表的**顺序**和**每项的大小**都是位置信息，错一个数字
 * 不报错、不崩溃，只会让其后每种字型都读到别人的字形（字是"能显示"，只是全是错的）。
 * 硬件上要靠人眼看字形对不对，很晚才会发现。
 *
 * 这不是假想：`feat/old_font_lib` 分支就是这么错的 —— 它按 `ST,FS,KT,HT` 排，
 * 而 5006048 实物映像的地理顺序是 `FS,HT,KT,ST`，代进累加式与实际基址只有 8/32
 * 命中（另外它用的单元大小还来自 `func.h` 里那批**未被引用且自相矛盾**的宏）。
 * 本用例把这张表钉死在实物映像上，这类错误会在 `make test` 阶段就红。
 *
 * 期望值是两个**互相独立**的来源，不是把被测代码的算法抄一遍：
 *   · 5006048 —— 原工程 P10_flip_BarBoard_BB/USER/FUNC/func.c 里 8 个读取函数
 *     `switch (fontType)` 的 32 个基址**字面量**（该工程配这块屏实机验证可用）
 *   · 3833024 —— 重构前 app_render.c 里 `ASC_UNIT/GBK_UNIT` 累加出来的偏移，
 *     即"把表搬到板级"这件事**不得改变任何一个地址**
 *
 * 换板/换字库母片时，这张表必须跟着换，且新期望值要来自该板实物映像的权威地址表 ——
 * 不要从 g_board_font 反推（那就成了自己验自己）。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_render.h" /* g_board_font / font_lib_desc_t */
#include "board.h"      /* BOARD_FONT_LIB_TOTAL_BYTES / CFG_CAP_CONTRACT */

typedef struct {
    uint8_t    size;
    font_enc_t cs;
    font_type_t type;
    uint32_t   off;
} font_expect_t;

/* 用 BOARD 的编译期量做板间判别 —— 测试按板级头分层的包含路径分板编译（-I $(BOARD_DIR)/<层>/Inc），
   新板加进来时这里会直接 #error 提示补表，而不是悄悄跳过核对。 */
#if BOARD_FONT_LIB_TOTAL_BYTES == 18518144U
/* ---- 5006048：GB2312，16/24/32/48，每组 FS,HT,KT,ST ---- */
static const font_expect_t s_expect[] = {
    {16, FONT_ENC_ASCII, FONT_FS,        0U}, /* 0 */
    {16, FONT_ENC_ASCII, FONT_HT,     2048U}, /* 2048 */
    {16, FONT_ENC_ASCII, FONT_KT,     4096U}, /* 4096 */
    {16, FONT_ENC_ASCII, FONT_ST,     6144U}, /* 6144 */
    {16, FONT_ENC_GBK, FONT_FS,     8192U}, /* 8192 */
    {16, FONT_ENC_GBK, FONT_HT,   290944U}, /* 290944 */
    {16, FONT_ENC_GBK, FONT_KT,   573696U}, /* 573696 */
    {16, FONT_ENC_GBK, FONT_ST,   856448U}, /* 856448 */
    {24, FONT_ENC_ASCII, FONT_FS,  1139200U}, /* 1139200 */
    {24, FONT_ENC_ASCII, FONT_HT,  1145344U}, /* 1145344 */
    {24, FONT_ENC_ASCII, FONT_KT,  1151488U}, /* 1151488 */
    {24, FONT_ENC_ASCII, FONT_ST,  1157632U}, /* 1157632 */
    {24, FONT_ENC_GBK, FONT_FS,  1163776U}, /* 1163776 */
    {24, FONT_ENC_GBK, FONT_HT,  1799968U}, /* 1799968 */
    {24, FONT_ENC_GBK, FONT_KT,  2436160U}, /* 2436160 */
    {24, FONT_ENC_GBK, FONT_ST,  3072352U}, /* 3072352 */
    {32, FONT_ENC_ASCII, FONT_FS,  3708544U}, /* 3708544 */
    {32, FONT_ENC_ASCII, FONT_HT,  3716736U}, /* 3716736 */
    {32, FONT_ENC_ASCII, FONT_KT,  3724928U}, /* 3724928 */
    {32, FONT_ENC_ASCII, FONT_ST,  3733120U}, /* 3733120 */
    {32, FONT_ENC_GBK, FONT_FS,  3741312U}, /* 3741312 */
    {32, FONT_ENC_GBK, FONT_HT,  4872320U}, /* 4872320 */
    {32, FONT_ENC_GBK, FONT_KT,  6003328U}, /* 6003328 */
    {32, FONT_ENC_GBK, FONT_ST,  7134336U}, /* 7134336 */
    {48, FONT_ENC_ASCII, FONT_FS,  8265344U}, /* 8265344 */
    {48, FONT_ENC_ASCII, FONT_HT,  8283776U}, /* 8283776 */
    {48, FONT_ENC_ASCII, FONT_KT,  8302208U}, /* 8302208 */
    {48, FONT_ENC_ASCII, FONT_ST,  8320640U}, /* 8320640 */
    {48, FONT_ENC_GBK, FONT_FS,  8339072U}, /* 8339072 */
    {48, FONT_ENC_GBK, FONT_HT, 10883840U}, /* 10883840 */
    {48, FONT_ENC_GBK, FONT_KT, 13428608U}, /* 13428608 */
    {48, FONT_ENC_GBK, FONT_ST, 15973376U}, /* 15973376 */
};
#elif BOARD_FONT_LIB_TOTAL_BYTES == 30713088U
/* ---- 3833024：GBK，14/16/20/24/32，每组 ST,FS,KT,HT ---- */
static const font_expect_t s_expect[] = {
    {14, FONT_ENC_ASCII, FONT_ST,        0U}, /* 0 */
    {14, FONT_ENC_ASCII, FONT_FS,     1344U}, /* 1344 */
    {14, FONT_ENC_ASCII, FONT_KT,     2688U}, /* 2688 */
    {14, FONT_ENC_ASCII, FONT_HT,     4032U}, /* 4032 */
    {14, FONT_ENC_GBK, FONT_ST,     5376U}, /* 5376 */
    {14, FONT_ENC_GBK, FONT_FS,   675696U}, /* 675696 */
    {14, FONT_ENC_GBK, FONT_KT,  1346016U}, /* 1346016 */
    {14, FONT_ENC_GBK, FONT_HT,  2016336U}, /* 2016336 */
    {16, FONT_ENC_ASCII, FONT_ST,  2686656U}, /* 2686656 */
    {16, FONT_ENC_ASCII, FONT_FS,  2688192U}, /* 2688192 */
    {16, FONT_ENC_ASCII, FONT_KT,  2689728U}, /* 2689728 */
    {16, FONT_ENC_ASCII, FONT_HT,  2691264U}, /* 2691264 */
    {16, FONT_ENC_GBK, FONT_ST,  2692800U}, /* 2692800 */
    {16, FONT_ENC_GBK, FONT_FS,  3458880U}, /* 3458880 */
    {16, FONT_ENC_GBK, FONT_KT,  4224960U}, /* 4224960 */
    {16, FONT_ENC_GBK, FONT_HT,  4991040U}, /* 4991040 */
    {20, FONT_ENC_ASCII, FONT_ST,  5757120U}, /* 5757120 */
    {20, FONT_ENC_ASCII, FONT_FS,  5760960U}, /* 5760960 */
    {20, FONT_ENC_ASCII, FONT_KT,  5764800U}, /* 5764800 */
    {20, FONT_ENC_ASCII, FONT_HT,  5768640U}, /* 5768640 */
    {20, FONT_ENC_GBK, FONT_ST,  5772480U}, /* 5772480 */
    {20, FONT_ENC_GBK, FONT_FS,  7208880U}, /* 7208880 */
    {20, FONT_ENC_GBK, FONT_KT,  8645280U}, /* 8645280 */
    {20, FONT_ENC_GBK, FONT_HT, 10081680U}, /* 10081680 */
    {24, FONT_ENC_ASCII, FONT_ST, 11518080U}, /* 11518080 */
    {24, FONT_ENC_ASCII, FONT_FS, 11522688U}, /* 11522688 */
    {24, FONT_ENC_ASCII, FONT_KT, 11527296U}, /* 11527296 */
    {24, FONT_ENC_ASCII, FONT_HT, 11531904U}, /* 11531904 */
    {24, FONT_ENC_GBK, FONT_ST, 11536512U}, /* 11536512 */
    {24, FONT_ENC_GBK, FONT_FS, 13260192U}, /* 13260192 */
    {24, FONT_ENC_GBK, FONT_KT, 14983872U}, /* 14983872 */
    {24, FONT_ENC_GBK, FONT_HT, 16707552U}, /* 16707552 */
    {32, FONT_ENC_ASCII, FONT_ST, 18431232U}, /* 18431232 */
    {32, FONT_ENC_ASCII, FONT_FS, 18437376U}, /* 18437376 */
    {32, FONT_ENC_ASCII, FONT_KT, 18443520U}, /* 18443520 */
    {32, FONT_ENC_ASCII, FONT_HT, 18449664U}, /* 18449664 */
    {32, FONT_ENC_GBK, FONT_ST, 18455808U}, /* 18455808 */
    {32, FONT_ENC_GBK, FONT_FS, 21520128U}, /* 21520128 */
    {32, FONT_ENC_GBK, FONT_KT, 24584448U}, /* 24584448 */
    {32, FONT_ENC_GBK, FONT_HT, 27648768U}, /* 27648768 */
};
#else
#error "新板：请为它补一份期望偏移表（来源应是该板实物字库映像的权威地址表）"
#endif

/* ---- 断言 ---- */
static int g_pass;
static int g_fail;

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  核心：累加偏移逐条等于实物映像基址
 * ================================================================ */

static void case_offsets_match_image(void)
{
    TEST_BEGIN("每个 (字号,编码,字型) 的累加偏移 == 实物映像基址");

    uint32_t off     = 0;
    uint16_t matched = 0;

    for (uint16_t i = 0; i < g_board_font.lib_count; i++) {
        const font_unit_t *u = &g_board_font.lib[i];

        /* 在期望表里找同键的一项 */
        const font_expect_t *want = NULL;
        for (size_t k = 0; k < sizeof(s_expect) / sizeof(s_expect[0]); k++) {
            if (s_expect[k].size == u->key.size && s_expect[k].cs == u->key.charset &&
                s_expect[k].type == u->key.type) {
                want = &s_expect[k];
                break;
            }
        }

        if (!want) {
            CHECK_MSG(0, "板级表多出一项 %u号/%u字型（期望表里没有）", (unsigned)u->key.size,
                      (unsigned)u->key.type);
        } else {
            CHECK_MSG(off == want->off, "%u号/%u字型 偏移 %u != 映像基址 %u（差 %+d）",
                      (unsigned)u->key.size, (unsigned)u->key.type, (unsigned)off,
                      (unsigned)want->off, (int)(off - want->off));
            matched++;
        }
        off += u->unit_size;
    }

    /* 期望表的每一条都必须被板上表命中 —— 否则是"板上缺了某个三元组"，
       而缺项在渲染时只会表现为"这个字不显示" */
    CHECK_MSG(matched == sizeof(s_expect) / sizeof(s_expect[0]), "期望表 %u 条，只命中 %u 条",
              (unsigned)(sizeof(s_expect) / sizeof(s_expect[0])), (unsigned)matched);

    /* 链尾必须正好落在映像末尾 */
    CHECK_MSG(off == BOARD_FONT_LIB_TOTAL_BYTES, "累加链尾 %u != BOARD_FONT_LIB_TOTAL_BYTES %u",
              (unsigned)off, (unsigned)BOARD_FONT_LIB_TOTAL_BYTES);
}

/* ================================================================
 *  结构性约束（都是"违反后不报错"的那类）
 * ================================================================ */

static void case_total_bytes_consistent(void)
{
    TEST_BEGIN("total_bytes 与编译期常量、与各项求和三者一致");

    /* 表内 total_bytes 与 board.h 的常量分居两处：不同步时配置区地址会算错，
       常量偏小会让配置区落进字库区、首次 save 的擦除直接毁字库 */
    CHECK_MSG(g_board_font.total_bytes == BOARD_FONT_LIB_TOTAL_BYTES,
              "表内 total_bytes %u != board.h 的 %u", (unsigned)g_board_font.total_bytes,
              (unsigned)BOARD_FONT_LIB_TOTAL_BYTES);

    uint32_t sum = 0;
    for (uint16_t i = 0; i < g_board_font.lib_count; i++)
        sum += g_board_font.lib[i].unit_size;
    CHECK_MSG(sum == g_board_font.total_bytes, "各项求和 %u != total_bytes %u", (unsigned)sum,
              (unsigned)g_board_font.total_bytes);
}

static void case_sizes_ascending(void)
{
    TEST_BEGIN("可用字号集合升序（最近邻回落依赖它）");

    CHECK_MSG(g_board_font.size_count > 0, "字号集合为空");
    for (uint8_t i = 1; i < g_board_font.size_count; i++) {
        CHECK_MSG(g_board_font.sizes[i] > g_board_font.sizes[i - 1],
                  "非升序：sizes[%u]=%u 不大于 sizes[%u]=%u", (unsigned)i,
                  (unsigned)g_board_font.sizes[i], (unsigned)(i - 1),
                  (unsigned)g_board_font.sizes[i - 1]);
    }

    /* 每个可用字号都必须真有 2 编码 × 4 字型 共 8 个单元 —— 缺项时
       _find_unit 找不到，该字直接不显示（不是崩溃，更难查） */
    for (uint8_t i = 0; i < g_board_font.size_count; i++) {
        const font_size_t sz     = g_board_font.sizes[i];
        int               found  = 0;
        for (uint16_t k = 0; k < g_board_font.lib_count; k++)
            if (g_board_font.lib[k].key.size == sz) found++;
        CHECK_MSG(found == 8, "%u 号只有 %d 个单元（应为 2 编码 × 4 字型 = 8）", (unsigned)sz,
                  found);
    }
}

static void case_fits_capacity_contract(void)
{
    TEST_BEGIN("字库 + 配置区放得进容量契约");

    /* 与 app_cfg_sched.h 的 _Static_assert 同一条件，这里再钉一次：
       改 board.h 的常量或板级表时，运行期门槛与编译期契约必须一起走 */
    CHECK_MSG(g_board_font.total_bytes + 8U * 4096U <= 32U * 1024U * 1024U,
              "字库 %u + 配置区放不进 32MB", (unsigned)g_board_font.total_bytes);
}

static void case_index_scheme(void)
{
    TEST_BEGIN("索引方案与字符集匹配（GB2312 的 8836 与 GBK 的 23940 不能互换）");

    if (g_board_font.gb_index == FONT_IDX_GB2312) {
        /* GB2312 单元必须正好是 8836 × 每字符字节，多一少一都会让后续单元整体错位 */
        for (uint16_t i = 0; i < g_board_font.lib_count; i++) {
            const font_unit_t *u = &g_board_font.lib[i];
            if (u->key.charset != FONT_ENC_GBK) continue;
            uint16_t bpc = (uint16_t)u->key.size * ((u->key.size + 7) / 8);
            CHECK_MSG(u->unit_size == 8836U * bpc, "%u号 GB 单元 %u != 8836 x %u", (unsigned)u->key.size,
                      (unsigned)u->unit_size, (unsigned)bpc);
        }
        CHECK_MSG(g_board_font.asc_index_base == 0x00 || g_board_font.asc_index_base == 0x20,
                  "ASCII 索引起点 %u 不是 0x00/0x20", (unsigned)g_board_font.asc_index_base);
    } else {
        for (uint16_t i = 0; i < g_board_font.lib_count; i++) {
            const font_unit_t *u = &g_board_font.lib[i];
            if (u->key.charset != FONT_ENC_GBK) continue;
            uint16_t bpc = (uint16_t)u->key.size * ((u->key.size + 7) / 8);
            CHECK_MSG(u->unit_size == 23940U * bpc, "%u号 GB 单元 %u != 23940 x %u",
                      (unsigned)u->key.size, (unsigned)u->unit_size, (unsigned)bpc);
        }
    }
}

/* ================================================================ */

int main(void)
{
    case_offsets_match_image();
    case_total_bytes_consistent();
    case_sizes_ascending();
    case_fits_capacity_contract();
    case_index_scheme();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
