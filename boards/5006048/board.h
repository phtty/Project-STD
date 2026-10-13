/**
 * @file    board.h
 * @brief   5006048 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR) 选中（Makefile 里排在板级头的第一位），
 * 因此共享代码统一写 \#include "board.h" 即可，无需条件编译。
 */

#pragma once

/* ---- IAP 配置记录是否存在 ----
 * **当前为 0，是临时状态**：本板以后要做 IAP，但现在的 IAP / Recovery App 不适配它
 * （板级引脚已变），所以暂时保持"无 IAP"的样子。
 *
 * 本板目前直烧 0x08000000、铺满 1024K，而 IAP 记录写死的地址是 0x08004000 ——
 * 那就在固件映像内部。做成 IAP 时要**同时**改三处（app_iap_cfg.c 里有编译期断言守着）：
 *     board.ld   FLASH_ORIGIN  0x08000000 → 0x08040000
 *     本文件      BOARD_VECT_TAB_OFFSET  0 → 0x40000
 *     本文件      BOARD_HAS_IAP_RECORD    0 → 1
 * 只翻 flag 不动布局会被那条断言当场拦下。 */
#define BOARD_HAS_IAP_RECORD 0 /**< 0 = 本板直烧、无 IAP 记录（临时状态） */

/* ---- 向量表偏移 ----
 * 必须与 boards/5006048/board.ld 的 FLASH_ORIGIN 保持一致：5006048 直烧 0x08000000，
 * 偏移 0；带 IAP bootloader 的板子则是 0x40000。两处不一致的表现是任何中断都
 * 跳到错误的地方（bootloader 的向量表或空白区），且不会有编译期报错。 */
#define BOARD_VECT_TAB_OFFSET 0x00000000UL /**< 向量表偏移；做 IAP 时改 0x40000，须与 board.ld 同步 */

/* ---- 外部字库总量 ----
 * 字库在 W25Qxx 上从地址 0 起线性连续排列，排在它之后的配置区要靠这个值算地址
 * （见 app_cfg_sched.h 的 _Static_assert 与 app_cfg_sched.c 的 s_ready 门槛）。
 *
 * **两板的字库不是同一版**：本板 GB2312、4 字号（16/24/32/48）；3833024 是
 * GBK、5 字号（14/16/20/24/32），总量 30713088。所以这是板级量，不能放共享头。
 *
 * 必须与 Application/Src/app_font_lib_board.c 的 g_board_font_lib.total_bytes 一致。不同步的后果
 * 不只是取字乱码 —— 常量偏小会让配置区落进字库区，首次 save 的扇区擦除直接
 * 毁掉字库。那个 .c 里有 _Static_assert 钉住，_render_init 里另有一条运行期校验。 */
#define BOARD_FONT_LIB_TOTAL_BYTES 18518144U /**< 本板 W25Qxx 字库总字节数，须与 g_board_font_lib.total_bytes 一致 */

/* ---- 整屏逻辑画布（app_screen）----
 *
 * 级联时主卡要画"整屏"，它比本卡那块屏大（其余由从卡显示）。1bpp 画布装得下，
 * 1 字节/像素装不下 —— 见 app_screen.c 的说明。
 *
 * 上限按**本板参与的最大级联规模**留：本板单卡 224×50，按 4 卡横排算 896×50
 * → ceil(896/8) × 50 = 112 × 50 = 5600 字节。运行期几何从中切。
 *
 * BOARD_SCREEN_CANVAS：**本板是双卡级联出货，必须是 1** —— 没有画布就没有"整屏"
 * 这个对象，主卡无从切分下发（下面「板级组合校验」会把 ROWS>1 且 CANVAS=0 拦下来）。
 * 单卡形态下它是纯开销（多一块 1bpp 缓冲 + 一次拷贝 + 把卡内多色塌缩成单色），
 * 那种场合才关掉。 */
#ifndef BOARD_SCREEN_CANVAS_MAX
#define BOARD_SCREEN_CANVAS_MAX (5600U) /**< 整屏逻辑画布上限（字节），按本板最大级联规模留 */
#endif
#ifndef BOARD_SCREEN_CANVAS
#define BOARD_SCREEN_CANVAS (1) /**< 1 = 启用整屏逻辑画布；本板双卡级联出货固定为 1 */
#endif
/* 本卡颜色（dev_display_color_t）。级联后由切分表逐卡给，这里只是单卡时的默认值。 */
#define BOARD_SCREEN_COLOR (2) /**< 本卡默认颜色：DEV_DISPLAY_COLOR_GREEN */

/* ---- 级联切分（整屏 = 若干张等尺寸卡按网格拼）----
 *
 * 本板单卡屏 224×50。整屏 = COLS 张卡横排 × ROWS 张卡竖排，每张卡占一整块
 * 224×50（卡的矩形尺寸就是本卡的屏几何，运行期从 dev_display_get() 读）。
 *
 * **本产品是两块卡上下拼（1×2），整屏 224×100**。1×1 是单卡直显形态，
 * 行为与没有级联之前逐字节相同 —— 单卡出货的板子才填 1。
 *
 * 网格只能表达**等尺寸卡**的规则拼法。异形拼法（一张卡占左半、另两张在右侧
 * 上下堆叠）目前表达不了。 */
#ifndef BOARD_CASC_COLS
#define BOARD_CASC_COLS (1) /**< 级联网格列数（横排卡数） */
#endif
#ifndef BOARD_CASC_ROWS
#define BOARD_CASC_ROWS (2) /**< 级联网格行数（竖排卡数） */
#endif

/* 主卡所在的**网格下标**（行优先：0 = 左上，1 = 右上，……）—— **出厂默认值**。
 *
 * **主卡不必在网格原点** —— 本产品的拼法是"上面一块、下面一块，**下面那块是主卡**"，
 * 那是 1×2 网格里的格号 1。所以地址不按网格下标编：主卡那格编 0，其余按行优先
 * 依次编 1、2、3…。两台设备的网格参数必须一致（它们描述的是**部署**）。
 *
 * **运行期可以改**（`app_screen_apply_identity`）：按键认领时"被按的那张卡"把
 * **它自己那一格**定为新的主卡格，各卡据此重算地址 —— 这就是"谁被按谁主卡"
 * 能成立的前提（只改地址不改格的话，被按的卡会去渲染另一块屏那一格，上下半幅对调）。
 * 改过的值随 `casc_id` 记录持久化，掉电不忘；这个宏只是"还没有记录时"的默认。 */
#ifndef BOARD_CASC_MASTER_CELL
#define BOARD_CASC_MASTER_CELL (1) /**< 主卡所在网格下标（行优先，1 = 右上；出厂默认） */
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
 *
 * 1bpp、ceil(屏宽/8)×屏高：本板单卡 224×50 → 28×50 = **1400**。
 * app_screen 的抽带缓冲与 app_casc 的从卡暂存都按它静态分配。
 * 必须 ≥ 实屏几何算出来的值 —— _screen_init 有运行期校验，不符会明确打出来。 */
#define BOARD_CASC_BAND_MAX (1400U) /**< 单卡矩形位图上限（1bpp，字节） */

/* ---- 级联总线地址（app_screen_self_addr）----
 * 0 = 主卡，1..0x1F = 从卡。
 *
 * **运行期事实，不写死在固件里**：两块卡烧**同一份固件**，谁是谁由三重来源决定，
 * 优先级 **拨码（仅 3833024）> W25Qxx 记录（`casc_id`）> 本宏（出厂默认）**。
 * 没有拨码的板子（本板）按 TEST 键认领 —— 被按的那张卡成为主卡并写记录。 */
/* ---- 本板地址是否由拨码决定 ----
 * 3833024 有 DIP1/DIP2（2bit=4 码，天然支持最多 4 卡）；5006048 没有拨码，靠 TEST 键
 * 认领 + W25Qxx 记录。级联的身份解析据此选"从哪读"（见 app_casc.c）。
 * **拨码优先于记录**：现场拨一下即生效，不用工具。 */
#ifndef BOARD_HAS_ADDR_DIP
#define BOARD_HAS_ADDR_DIP (0) /**< 0 = 本板无拨码，地址靠 TEST 键认领 + W25Qxx 记录 */
#endif

/* ---- 出厂默认身份：**主卡**（0 = 主卡）----
 *
 * 出厂（还没有记录）时，板子认为自己在**主卡格**（见 BOARD_CASC_MASTER_CELL）。
 * 于是与产品排布自洽：两块出厂板都认为自己是下面那块主卡，按**下面那块**的 TEST
 * 即完成配对（另一块收到广播后让到另一格）；一块板单独上电也是一台可用设备。
 *
 * 反过来（默认从卡）在"两张都自称主卡"还解不开的年代更安全 —— 那个死局已经修好
 * （认领主卡时会广播让位，见 app_casc.c），所以这里回到主卡。 */
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
