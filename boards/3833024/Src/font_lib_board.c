/**
 * @file    font_lib_board.c
 * @brief   3833024 的板级字库描述 —— GBK 字符集，5 字号 × 2 编码 × 4 字型
 *
 * **内容与拆分前 app_render.c 里的 g_font_lib[] 逐条相同**，只是搬了位置：
 * 这次改动是把"字库长什么样"从共享层挪到板级，不是改字库。
 *
 * 两个字库模型（本板与 5006048）的差异：
 *   · 字号集合   本板 14/16/20/24/32；5006048 是 16/24/32/48
 *   · 字符集     本板 GBK（23940 字）；5006048 是 GB2312（8836 字 = 94×94 区位）
 *   · ASCII 起点 本板 0x20（96 槽）；5006048 是 0x00（128 槽，按原始码索引）
 *   · 单元顺序   本板每组 ST,FS,KT,HT；5006048 的实物映像是 FS,HT,KT,ST
 */

#include "app_render.h"
#include "board.h"

/* ---- 单元字节数 ----
 * ASCII = (size/2)宽 × size高 × 96 字; GBK = size宽 × size高 × 23940 字 */
#define N_ASC_CHARS (96U)
#define N_GBK_CHARS (23940U)

#define ASC_UNIT(sz) ((uint32_t)(sz) * (((sz) / 2 + 7) / 8) * N_ASC_CHARS)
#define GBK_UNIT(sz) ((uint32_t)(sz) * (((sz) + 7) / 8) * N_GBK_CHARS)

/* 本板可用字号，**必须升序**（app_render.c 的最近邻回落依赖它）。
 * 没有 48 号 —— 请求 48 号会回落到 32 号。 */
static const font_size_t s_sizes[] = {FONT_14, FONT_16, FONT_20, FONT_24, FONT_32};

/* 字库描述表 — **顺序必须与 Flash 中字库单元的排列一致**。
 * 第 i 项的 Flash 起始偏移 = 前 i 项 unit_size 之和（线性连续，从地址 0 起）。 */
static const font_unit_t s_lib[] = {
    /* 14号 */
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(14)},
    /* 16号 */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(16)},
    /* 20号 */
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(20)},
    /* 24号 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(24)},
    /* 32号 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(32)},
};

const font_lib_desc_t g_board_font = {
    .lib            = s_lib,
    .lib_count      = sizeof(s_lib) / sizeof(s_lib[0]),
    .sizes          = s_sizes,
    .size_count     = sizeof(s_sizes) / sizeof(s_sizes[0]),
    .asc_index_base = 0x20, /* 96 槽，ch - 0x20 */
    .gb_index       = FONT_IDX_GBK,
    .total_bytes    = BOARD_FONT_LIB_TOTAL_BYTES,
};

/* 表内 total_bytes 直接取编译期常量，所以两者不可能不一致；但 lib[] 的实际求和
 * 是否等于它是另一回事（写错一个数字就会错位）—— 那条由 _render_init 运行期校验。 */
_Static_assert(BOARD_FONT_LIB_TOTAL_BYTES == 30713088U,
               "3833024 font total changed - board.h and s_lib[] must be updated together");
