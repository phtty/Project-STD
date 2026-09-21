/**
 * @file    font_lib_board.c
 * @brief   5006048 的板级字库描述 —— GB2312 字符集，4 字号 × 2 编码 × 4 字型
 *
 * 与 3833024 那份**不是同一版字库**，四处都不同：
 *   · 字号集合   本板 16/24/32/48；3833024 是 14/16/20/24/32
 *   · 字符集     本板 GB2312（8836 字 = 94×94 区位全集）；3833024 是 GBK（23940 字）
 *   · ASCII 起点 本板 0x00（128 槽，按原始码索引）；3833024 是 0x20（96 槽）
 *   · 单元顺序   本板每组 FS,HT,KT,ST；3833024 是 ST,FS,KT,HT
 *
 * ---- 地址来源与验证 ----
 * 地址与几何取自实物映像的**权威来源**：原工程 P10_flip_BarBoard_BB 的
 * USER/FUNC/func.c（配这块屏实机验证可用）。该文件里 8 个读取函数各自
 * `switch (fontType)` 给出 4 个字型的基址，全部是整数字面量。
 *
 * **注意不要参考 func.h 里那 32 个 `*_ADDRESS` 宏** —— 它们描述的是另一版映像布局：
 * 隐含的每字型步长（2080/6176/8224/18464）除以该字型的每字符字节数都不是整数，
 * 字型根本平铺不开；而且全写成 `N / 4096`，C 里是整数除法，`2080/4096` 与 `0/4096`
 * 撞成同一个值。它们在原工程里也**一处都没被引用**（全在注释里）。
 *
 * 本表的正确性由两点保证，缺一不可：
 *   1. 顺序 = 实物映像的地理顺序 (FS,HT,KT,ST)。累加求偏移对顺序敏感，错了不报错、
 *      只会让每种字型取到别人的字形。旧 feat/old_font_lib 分支就踩在这上面。
 *   2. 每项 unit_size 与映像一致（ASCII 128 槽、GB2312 8836 槽）。
 * 两者合起来，累加结果**逐条等于** func.c 里的 32 个基址 —— 已用脚本全量核对
 * （32/32 命中，链尾 18518144 与映像尾部严丝合缝）。
 */

#include "app_render.h"
#include "board.h"

/* ---- 单元字节数 ----
 * ASCII    = (size/2)宽 × size高 × 128 槽   ← 按**原始码**索引，不是 0x20 起
 * GB2312   = size宽 × size高 × 8836 字      ← 94×94 区位全集 */
#define N_ASC_CHARS    (128U)
#define N_GB2312_CHARS (8836U)

#define ASC_UNIT(sz)    ((uint32_t)(sz) * (((sz) / 2 + 7) / 8) * N_ASC_CHARS)
#define GB2312_UNIT(sz) ((uint32_t)(sz) * (((sz) + 7) / 8) * N_GB2312_CHARS)

/* 本板可用字号，**必须升序**（app_render.c 的最近邻回落依赖它）。
 * 没有 14/20 号 —— 请求它们会回落到 16 号。 */
static const font_size_t s_sizes[] = {FONT_16, FONT_24, FONT_32, FONT_48};

/* 字库描述表 — **顺序必须与 Flash 中字库单元的排列一致**（见文件头的说明）。
 * 第 i 项的 Flash 起始偏移 = 前 i 项 unit_size 之和（线性连续，从地址 0 起）。
 *
 * 每组内部的地理顺序是 FS → HT → KT → ST，与 3833024 那份的 ST→FS→KT→HT 相反。 */
static const font_unit_t s_lib[] = {
    /* 16号 — 块起点 0 */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(16)},          /* 0       */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(16)},          /* 2048    */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(16)},          /* 4096    */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(16)},          /* 6144    */
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_FS}, GB2312_UNIT(16)},         /* 8192    */
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_HT}, GB2312_UNIT(16)},         /* 290944  */
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_KT}, GB2312_UNIT(16)},         /* 573696  */
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_ST}, GB2312_UNIT(16)},         /* 856448  */
    /* 24号 — 块起点 1139200 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(24)},          /* 1139200 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(24)},          /* 1145344 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(24)},          /* 1151488 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(24)},          /* 1157632 */
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_FS}, GB2312_UNIT(24)},         /* 1163776 */
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_HT}, GB2312_UNIT(24)},         /* 1799968 */
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_KT}, GB2312_UNIT(24)},         /* 2436160 */
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_ST}, GB2312_UNIT(24)},         /* 3072352 */
    /* 32号 — 块起点 3708544 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(32)},          /* 3708544 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(32)},          /* 3716736 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(32)},          /* 3724928 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(32)},          /* 3733120 */
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_FS}, GB2312_UNIT(32)},         /* 3741312 */
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_HT}, GB2312_UNIT(32)},         /* 4872320 */
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_KT}, GB2312_UNIT(32)},         /* 6003328 */
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_ST}, GB2312_UNIT(32)},         /* 7134336 */
    /* 48号 — 块起点 8265344 */
    {{.size = 48, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(48)},          /* 8265344 */
    {{.size = 48, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(48)},          /* 8283776 */
    {{.size = 48, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(48)},          /* 8302208 */
    {{.size = 48, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(48)},          /* 8320640 */
    {{.size = 48, .charset = FONT_ENC_GBK, .type = FONT_FS}, GB2312_UNIT(48)},         /* 8339072 */
    {{.size = 48, .charset = FONT_ENC_GBK, .type = FONT_HT}, GB2312_UNIT(48)},         /* 10883840 */
    {{.size = 48, .charset = FONT_ENC_GBK, .type = FONT_KT}, GB2312_UNIT(48)},         /* 13428608 */
    {{.size = 48, .charset = FONT_ENC_GBK, .type = FONT_ST}, GB2312_UNIT(48)},         /* 15973376 */
};

const font_lib_desc_t g_board_font = {
    .lib            = s_lib,
    .lib_count      = sizeof(s_lib) / sizeof(s_lib[0]),
    .sizes          = s_sizes,
    .size_count     = sizeof(s_sizes) / sizeof(s_sizes[0]),
    .asc_index_base = 0x00, /* 128 槽，ch 原值索引 */
    /* charset 字段仍写 FONT_ENC_GBK —— 它表示"双字节汉字字库"这一路，不是 GBK 编码。
       输入文本经 UTF8ToGBK 转成 GBK 后，**再按本板的区位方案映射到 GB2312 字形**。
       代价：GBK 有而 GB2312 没有的字（生僻字、部分符号）会落到区位界外，
       _char_addr 返回 false 跳过该字 —— 这是本板字库的固有边界，不是缺陷。 */
    .gb_index       = FONT_IDX_GB2312,
    .total_bytes    = BOARD_FONT_LIB_TOTAL_BYTES,
};

/* 表内 total_bytes 直接取编译期常量，两者不可能不一致；lib[] 的实际求和是否等于它
 * 是另一回事（写错一个数字就整体错位）—— 那条由 _render_init 运行期校验。 */
_Static_assert(BOARD_FONT_LIB_TOTAL_BYTES == 18518144U,
               "5006048 font total changed - board.h and s_lib[] must be updated together");
