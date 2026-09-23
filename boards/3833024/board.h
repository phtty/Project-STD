/**
 * @file    board.h
 * @brief   3833024 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR) 选中（Makefile 里排在板级头的第一位），
 * 因此共享代码统一写 #include "board.h" 即可，无需条件编译。
 */

#pragma once

/* ---- IAP 配置记录是否存在 ----
 * 只有带 IAP bootloader 的板子才有"0x08004000 处的配置记录"这回事：bootloader 占
 * Sector 0~3、应用从 0x08040000 起，Sector 1 留作记录（见 board.ld 的 FLASH 起始）。
 * 直烧的板子固件从 0x08000000 起铺满整片，**0x08004000 就在固件映像内部** ——
 * 那里没有记录，对它做任何擦写都是在抹自己的代码。详见 app_iap_cfg.c 的说明。 */
#define BOARD_HAS_IAP_RECORD 1

/* ---- 向量表偏移 ----
 * 必须与 boards/3833024/board.ld 的 FLASH_ORIGIN 保持一致：3833024 带 IAP bootloader，
 * 主固件从 0x08040000 起，偏移 0x40000；直烧的板子是 0。两处不一致的表现是任何
 * 中断都跳到错误的地方，且不会有编译期报错。 */
#define BOARD_VECT_TAB_OFFSET 0x00040000UL

/* ---- 外部字库总量 ----
 * 字库在 W25Qxx 上从地址 0 起线性连续排列，排在它之后的配置区要靠这个值算地址
 * （见 app_cfg_sched.h 的 _Static_assert 与 app_cfg_sched.c 的 s_ready 门槛）。
 *
 * **两板的字库不是同一版**：本板 GBK、5 字号（14/16/20/24/32）；5006048 是
 * GB2312、4 字号（16/24/32/48），总量 18518144。所以这是板级量，不能放共享头。
 *
 * 必须与 Src/app_font_lib_board.c 的 g_board_font_lib.total_bytes 一致。不同步的后果
 * 不只是取字乱码 —— 常量偏小会让配置区落进字库区，首次 save 的扇区擦除直接
 * 毁掉字库。那个 .c 里有 _Static_assert 钉住，_render_init 里另有一条运行期校验。 */
#define BOARD_FONT_LIB_TOTAL_BYTES 30713088U

/* ---- 整屏逻辑画布（app_screen）----
 *
 * 级联时主卡要画"整屏"，它比本卡那块屏大（其余由从卡显示）。1bpp 画布装得下，
 * 1 字节/像素装不下 —— 见 app_screen.c 的说明。
 *
 * 上限按**本板参与的最大级联规模**留：本板编的 P20 16×8 模组屏是 128×32，
 * 按 4 卡横排算 512×32 → ceil(512/8) × 32 = 64 × 32 = 2048 字节。运行期几何从中切。
 *
 * BOARD_SCREEN_CANVAS 默认 **0**：单独一块卡上画布是纯开销 —— 多占一块缓冲、
 * 多一次拷贝、还要把卡内多色塌缩成单色，却没换来任何功能。等级联落地时打开。
 * 中间随时可以 `app_render_set_target(NULL)` 完全回退。 */
#ifndef BOARD_SCREEN_CANVAS_MAX
#define BOARD_SCREEN_CANVAS_MAX (2048U)
#endif
#ifndef BOARD_SCREEN_CANVAS
#define BOARD_SCREEN_CANVAS     (0)
#endif
/* 本卡颜色（dev_display_color_t）。级联后由切分表逐卡给，这里只是单卡时的默认值。 */
#define BOARD_SCREEN_COLOR      (2) /* COLOR_GREEN */

/* ---- 级联切分（整屏 = 若干张等尺寸卡按网格拼）----
 * 与 5006048 同名同义，说明见那边的 board.h。本板单卡屏 128×32，
 * 整屏 = 128×COLS 宽 × 32×ROWS 高，地址 = 网格下标（行优先，0 = 左上 = 主卡）。
 * 默认 1×1 = 单卡直显，行为与没有级联之前逐字节相同。 */
#ifndef BOARD_CASC_COLS
#define BOARD_CASC_COLS      (1)
#endif
#ifndef BOARD_CASC_ROWS
#define BOARD_CASC_ROWS      (1)
#endif

/* 主卡所在的**网格下标**（行优先：0 = 左上）。主卡不必在原点 —— 现场的拼法就有
 * "上面一块、下面一块，下面那块是主卡"，那是 1×2 网格里的格号 1。
 * 详见 5006048/board.h 的同名宏。 */
#ifndef BOARD_CASC_MASTER_CELL
#define BOARD_CASC_MASTER_CELL (0)
#endif

/* ---- 本板跑不跑级联：**由网格形状推导**（单卡 = COLS×ROWS == 1 = 不跑）----
 *
 * 单卡板上整个 `app_casc.c` 编译成空（不占 Flash、不占那 5KB CCMRAM、不发每 10 秒
 * 一次的 PING、也不读身份记录与拨码 —— 与"没有级联之前的那套功能"逐字一致）；
 * 连带 `app_factory_test.c` 也不再 include 级联头，于是单卡板的构建**不需要**
 * `Application/Inc/CASC` 出现在包含路径上。
 *
 * 测试套件要跑级联：在包含任何头之前 `#define BOARD_CASC_ENABLED 1`。 */
#ifndef BOARD_CASC_ENABLED
#define BOARD_CASC_ENABLED (((BOARD_CASC_COLS) * (BOARD_CASC_ROWS)) > 1)
#endif

/* ---- 单卡矩形位图的上限（字节）----
 * 1bpp、ceil(屏宽/8)×屏高：本板单卡 128×32 → 16×32 = **512**。
 * app_screen 的抽带缓冲与 app_casc 的从卡暂存都按它静态分配。 */
#define BOARD_CASC_BAND_MAX  (512U)

/* ---- 级联总线地址（app_screen_self_addr）----
 * 0 = 主卡，1..0x1F = 从卡。**本期是编译期常量**，即主卡与从卡烧不同固件；
 * 后续期由 W25Qxx 的切分表记录覆盖 —— 同型号板子可以是 2/3/4 卡部署，
 * 那是部署期事实，不可能永久写死在固件里。
 *
 * 注意 3833024 有 DIP1/DIP2 可以直接读出地址（2 bit = 4 码，恰好 1 主 + 3 从），
 * 而 5006048 只有 KEY_TST、没有拨码 —— 所以本参数不能做成"必须靠硬件读"，
 * 否则得给 5006048 改板。 */
/* ---- 本板地址是否由拨码决定 ----
 * 3833024 有 DIP1/DIP2（2bit=4 码，天然支持最多 4 卡）；5006048 没有拨码，靠 TEST 键
 * 认领 + W25Qxx 记录。级联的身份解析据此选"从哪读"（见 app_casc.c）。
 * **拨码优先于记录**：现场拨一下即生效，不用工具。 */
#ifndef BOARD_HAS_ADDR_DIP
#define BOARD_HAS_ADDR_DIP (1)
#endif

#ifndef BOARD_CASC_ADDR
#define BOARD_CASC_ADDR      (0) /* 0 = 主卡 */
#endif

/* ---- 板级组合校验（放在最后：上面几个宏都要已定义）---- */

/* 多卡必须有整屏画布：没有画布就没有"整屏"这个对象，主卡无从切分下发。
 * 这里拦下而不是运行期静默降级 —— 降级的表现是"配了多卡但只有主卡自己那块屏动"。 */
#if BOARD_SCREEN_CANVAS == 0 && (BOARD_CASC_COLS > 1 || BOARD_CASC_ROWS > 1)
#error "多卡级联需要 BOARD_SCREEN_CANVAS=1（整屏画布）"
#endif

/* "画布池装得下整屏"不在这里判：那要用**实屏几何**（由所选的显示模组驱动决定，
 * 可以用 board.mk 换），board.h 里只有单卡尺寸这个事实，写死会与换屏脱节。
 * _screen_init 有该检查，用的是运行期几何，不符会明确打出来并停用门面。 */
