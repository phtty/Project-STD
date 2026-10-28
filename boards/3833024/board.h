/**
 * @file    board.h
 * @brief   3833024 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR) 选中（Makefile 里排在板级头的第一位），
 * 因此共享代码统一写 \#include "board.h" 即可，无需条件编译。
 */

#pragma once

/* ---- bootloader 开关 ----
 * 1 = 本板带 IAP bootloader（默认）：应用从 0x08040000 起，前面 256KB 留给
 *     bootloader，Sector 1 留作配置记录。
 * 0 = 直烧板（板上没有升级程序）：应用从 0x08000000 起铺满整片。
 *
 * **必须在编译时定义，不能只改这里**：链接脚本的 FLASH_ORIGIN 也要跟着变。
 *   make BOOTLOADER=0            ← Makefile 会自动加 -D 和 --defsym
 *   EIDE：预定义宏加 BOARD_HAS_IAP_BOOTLOADER=0，
 *         链接参数再加 -Wl,--defsym=APP_FLASH_ORIGIN=0x08000000
 *                        -Wl,--defsym=APP_FLASH_LENGTH=0x100000
 *
 * 两处不一致的表现：任何中断都跳到错误地址，且不会有编译期报错。 */
#ifndef BOARD_HAS_IAP_BOOTLOADER
#define BOARD_HAS_IAP_BOOTLOADER 1
#endif

#if BOARD_HAS_IAP_BOOTLOADER
/**< 1 = 本板带 IAP bootloader，0x08004000 处有配置记录 */
#define BOARD_HAS_IAP_RECORD 1
/**< 向量表偏移，须与 board.ld 的 FLASH_ORIGIN 一致 */
#define BOARD_VECT_TAB_OFFSET 0x00040000UL
#else
#define BOARD_HAS_IAP_RECORD 0
#define BOARD_VECT_TAB_OFFSET 0x00000000UL
#endif

/* ---- 外部字库总量 ----
 * 字库在 W25Qxx 上从地址 0 起线性连续排列，排在它之后的配置区要靠这个值算地址
 * （见 app_cfg_sched.h 的 static_assert 与 app_cfg_sched.c 的 s_ready 门槛）。
 *
 * **两板的字库不是同一版**：本板 GBK、5 字号（14/16/20/24/32）；5006048 是
 * GB2312、4 字号（16/24/32/48），总量 18518144。所以这是板级量，不能放共享头。
 *
 * 必须与 Application/Src/app_font_lib_board.c 的 g_board_font_lib.total_bytes 一致。不同步的后果
 * 不只是取字乱码 —— 常量偏小会让配置区落进字库区，首次 save 的扇区擦除直接
 * 毁掉字库。那个 .c 里有 static_assert 钉住，_render_init 里另有一条运行期校验。 */
#define BOARD_FONT_LIB_TOTAL_BYTES 30713088U /**< 本板 W25Qxx 字库总字节数，须与 g_board_font_lib.total_bytes 一致 */

/* ---- 整屏逻辑画布（app_screen）----
 *
 * 级联时主卡要画"整屏"，它比本卡那块屏大（其余由从卡显示）。1bpp 画布装得下，
 * 1 字节/像素装不下 —— 见 app_screen.c 的说明。
 *
 * 本产品整屏 144×48，画布要 ceil(144/8) × 48 = 18 × 48 = **864 字节**。
 * 上限取 2048：留给"再加卡/再加模组"换形，不把这一个数写死成刚好够 —— 换形时
 * 要一起改的就不止它（切分表、单卡位图上限、模组宏都在同一张图上）。
 *
 * BOARD_SCREEN_CANVAS 本板**开**：本产品就是双卡级联，主卡没有整屏画布就无从
 * 切分下发（文件末尾的组合校验会直接 #error）。
 * 中间随时可以 `app_render_set_target(NULL)` 完全回退。 */
#ifndef BOARD_SCREEN_CANVAS_MAX
#define BOARD_SCREEN_CANVAS_MAX (2048U) /**< 整屏逻辑画布上限（字节）；本产品需 864，余量留给换形 */
#endif
#ifndef BOARD_SCREEN_CANVAS
#define BOARD_SCREEN_CANVAS (1) /**< 1 = 启用整屏逻辑画布；本板双卡级联，必须开 */
#endif
/* 本卡颜色（dev_display_color_t）。级联后由切分表逐卡给，这里只是单卡时的默认值。 */
#define BOARD_SCREEN_COLOR (2) /**< 本卡默认颜色：DEV_DISPLAY_COLOR_GREEN */

/* ---- 一块卡带几个显示模组（本产品的几何先看这里）----
 *
 * 整屏 6 个 48×24 模组（2 行 × 3 列 = 144×48），两张卡**不等宽**：
 *
 *   格 0（左，从卡）：1 列 × 2 行 = 2 个模组 → 本地画面 48×48
 *   格 1（右，主卡）：2 列 × 2 行 = 4 个模组 → 本地画面 96×48
 *
 * 两张卡同型号、同一份源码，只靠这两个宏配数量。模组序号按**列优先**：
 * m = 列 * ROWS + 行，通道 ch = 2m + 组号（组 0 = 上 16 行、组 1 = 下 8 行）；
 * 所有模组共用 CLK/LAT/OE/A-B，每个模组占一对 RGB。
 *
 * **默认值是主卡**（2×2）。从卡构建时两个宏都要换：
 *   make MODCOLS=1 MODROWS=2       （Makefile 转成 -DBOARD_DISPLAY_MODULE_*）
 *   EIDE：目标设置的预定义宏改成 BOARD_DISPLAY_MODULE_COLS=1 / ..._ROWS=2
 * 改错不会有编译错误，表现是模组错位 / 半屏黑 —— 必须与实卡接线一致。 */
#ifndef BOARD_DISPLAY_MODULE_COLS
#define BOARD_DISPLAY_MODULE_COLS (2) /**< 每卡横向模组数（本产品的从卡配 1） */
#endif
#ifndef BOARD_DISPLAY_MODULE_ROWS
#define BOARD_DISPLAY_MODULE_ROWS (2) /**< 每卡纵向模组数（两张卡都是 2） */
#endif

/* ---- 级联网格（几张卡）----
 * 网格只表达**卡数**（COLS×ROWS：地址集合、级联开关都由它推导），
 * 不表达每张卡的矩形 —— 本产品两卡不等宽，等尺寸网格算不出来。
 * 真实矩形在板级切分表：boards/3833024/Application/Src/app_screen_board.c
 * （`app_screen_board_cells()`；共享代码里是弱定义，没有切分表的板回落网格）。
 * 与 5006048 同名同义，说明见那边的 board.h。 */
#ifndef BOARD_CASC_COLS
#define BOARD_CASC_COLS (2) /**< 级联网格列数（横排卡数） */
#endif
#ifndef BOARD_CASC_ROWS
#define BOARD_CASC_ROWS (1) /**< 级联网格行数（竖排卡数） */
#endif

/* 主卡所在的**网格下标**（行优先：0 = 左上）。主卡不必在原点 —— 现场的拼法就有
 * "上面一块、下面一块，下面那块是主卡"，那是 1×2 网格里的格号 1。
 * 本产品是 2×1 横排、**右边那张（格 1）是主卡**；切分表与地址编号都按它。
 * 详见 5006048/board.h 的同名宏。 */
#ifndef BOARD_CASC_MASTER_CELL
#define BOARD_CASC_MASTER_CELL (1) /**< 主卡所在网格下标（行优先，0 = 左上） */
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
#define BOARD_CASC_ENABLED (((BOARD_CASC_COLS) * (BOARD_CASC_ROWS)) > 1) /**< 1 = 本板跑级联（单卡时为 0） */
#endif

/* ---- 单卡矩形位图的上限（字节）----
 * 1bpp、ceil(卡宽/8)×卡高。本产品最大那块是主卡：96×48 → 12×48 = **576**
 * （从卡 48×48 → 6×48 = 288）。上限取 640，留 64 字节余量 —— 换模组/换拼法时
 * 不至于一改就踩线，但也不留到能藏住"几何算错"的程度。
 * app_screen 的抽带缓冲与 app_casc 的从卡暂存都按它静态分配；运行期几何真超了
 * 它，`_apply_identity` 会打出来并停用门面（不会静默截断）。 */
#define BOARD_CASC_BAND_MAX (640U) /**< 单卡矩形位图上限（1bpp，字节）；最大卡 96×48 = 576 */

/* ---- 级联总线地址（app_screen_self_addr）----
 * 0 = 主卡，1..0x1F = 从卡。
 *
 * **运行期事实，不写死在固件里**：地址（谁主谁从）由三重来源决定，
 * 优先级 **拨码（本板有）> W25Qxx 记录（`casc_id`）> 本宏（出厂默认）**。
 * ⚠️ 但**几何**是编译期事实：主卡 2×2 模组、从卡 1×2，所以两份固件不能互换
 * （见上面 `BOARD_DISPLAY_MODULE_*`）。能互换的是"同一卡型的任意两张"，
 * 它们靠拨码/记录区分地址。
 * 没有拨码的板子（5006048）按 TEST 键认领 —— 被按的那张卡成为主卡并写记录。
 *
 * 注意 3833024 有 DIP1/DIP2 可以直接读出地址（2 bit = 4 码，恰好 1 主 + 3 从），
 * 而 5006048 只有 KEY_TST、没有拨码 —— 所以本参数不能做成"必须靠硬件读"，
 * 否则得给 5006048 改板。 */
/* ---- 本板地址是否由拨码决定 ----
 * 3833024 有 DIP1/DIP2（2bit=4 码，天然支持最多 4 卡）；5006048 没有拨码，靠 TEST 键
 * 认领 + W25Qxx 记录。级联的身份解析据此选"从哪读"（见 app_casc.c）。
 * **拨码优先于记录**：现场拨一下即生效，不用工具。 */
#ifndef BOARD_HAS_ADDR_DIP
#define BOARD_HAS_ADDR_DIP (1) /**< 1 = 本板地址由拨码决定（拨码优先于记录） */
#endif

#ifndef BOARD_CASC_ADDR
#define BOARD_CASC_ADDR (0) /**< 出厂默认总线地址（0 = 主卡） */
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
