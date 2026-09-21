/**
 * @file    board.h
 * @brief   5006048 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR) 选中（Makefile 里排在板级头的第一位），
 * 因此共享代码统一写 #include "board.h" 即可，无需条件编译。
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
#define BOARD_HAS_IAP_RECORD 0

/* ---- 向量表偏移 ----
 * 必须与 boards/5006048/board.ld 的 FLASH_ORIGIN 保持一致：5006048 直烧 0x08000000，
 * 偏移 0；带 IAP bootloader 的板子则是 0x40000。两处不一致的表现是任何中断都
 * 跳到错误的地方（bootloader 的向量表或空白区），且不会有编译期报错。 */
#define BOARD_VECT_TAB_OFFSET 0x00000000UL /* 做 IAP 时改 0x40000，与 board.ld 同步 */

/* ---- 外部字库总量 ----
 * 字库在 W25Qxx 上从地址 0 起线性连续排列，排在它之后的配置区要靠这个值算地址
 * （见 app_cfg_sched.h 的 _Static_assert 与 app_cfg_sched.c 的 s_ready 门槛）。
 *
 * **两板的字库不是同一版**：本板 GB2312、4 字号（16/24/32/48）；3833024 是
 * GBK、5 字号（14/16/20/24/32），总量 30713088。所以这是板级量，不能放共享头。
 *
 * 必须与 Src/font_lib_board.c 的 g_board_font.total_bytes 一致。不同步的后果
 * 不只是取字乱码 —— 常量偏小会让配置区落进字库区，首次 save 的扇区擦除直接
 * 毁掉字库。那个 .c 里有 _Static_assert 钉住，_render_init 里另有一条运行期校验。 */
#define BOARD_FONT_LIB_TOTAL_BYTES 18518144U

/* ---- 整屏逻辑画布（app_screen）----
 *
 * 级联时主卡要画"整屏"，它比本卡那块屏大（其余由从卡显示）。1bpp 画布装得下，
 * 1 字节/像素装不下 —— 见 app_screen.c 的说明。
 *
 * 上限按**本板参与的最大级联规模**留：本板单卡 224×50，按 4 卡横排算 896×50
 * → ceil(896/8) × 50 = 112 × 50 = 5600 字节。运行期几何从中切。
 *
 * BOARD_SCREEN_CANVAS 默认 **0**：单独一块卡上画布是纯开销 —— 多占一块缓冲、
 * 多一次拷贝、还要把卡内多色塌缩成单色，却没换来任何功能。等级联落地时打开。
 * 中间随时可以 `app_render_set_target(NULL)` 完全回退。 */
#ifndef BOARD_SCREEN_CANVAS_MAX
#define BOARD_SCREEN_CANVAS_MAX (5600U)
#endif
#ifndef BOARD_SCREEN_CANVAS
#define BOARD_SCREEN_CANVAS (0)
#endif
/* 本卡颜色（display_color_t）。级联后由切分表逐卡给，这里只是单卡时的默认值。 */
#define BOARD_SCREEN_COLOR (2) /* COLOR_GREEN */

/* ---- 级联切分（整屏 = 若干张等尺寸卡按网格拼）----
 *
 * 本板单卡屏 224×50。整屏 = COLS 张卡横排 × ROWS 张卡竖排，每张卡占一整块
 * 224×50（卡的矩形尺寸就是本卡的屏几何，运行期从 dev_display_get() 读）。
 *
 * **1×1 = 单卡直显，行为与没有级联之前逐字节相同** —— 这是默认值，也是现场
 * 单卡出货的形态。做多卡时改这两个数（暂时如此；后续期由 W25Qxx 的切分表记录
 * 覆盖 —— 同型号板子可以是 2/3/4 卡部署，那是部署期事实，不该永久编译进固件）。
 *
 * 网格只能表达**等尺寸卡**的规则拼法。异形拼法（一张卡占左半、另两张在右侧
 * 上下堆叠）要等切分表记录那一期。 */
#ifndef BOARD_CASCADE_COLS
#define BOARD_CASCADE_COLS (1)
#endif
#ifndef BOARD_CASCADE_ROWS
#define BOARD_CASCADE_ROWS (1)
#endif

/* 主卡所在的**网格下标**（行优先：0 = 左上，1 = 右上，……）。
 *
 * **主卡不必在网格原点** —— 现场的拼法就有"上面一块、下面一块，**下面那块是主卡**"，
 * 那是 1×2 网格里的格号 1。所以地址不按网格下标编：主卡那格编 0，其余按行优先
 * 依次编 1、2、3…。两台设备的 COLS/ROWS/MASTER_CELL 必须一致（它们描述的是
 * **部署**），只有 BOARD_CASCADE_ADDR 各填各的。
 *
 * 这个宏只影响"哪个格号编成地址 0"与 1..N 的次序，**不影响**整屏几何与各卡矩形。 */
#ifndef BOARD_CASCADE_MASTER_CELL
#define BOARD_CASCADE_MASTER_CELL (0)
#endif

/* ---- 单卡矩形位图的上限（字节）----
 *
 * 1bpp、ceil(屏宽/8)×屏高：本板单卡 224×50 → 28×50 = **1400**。
 * app_screen 的抽带缓冲与 app_cascade 的从卡暂存都按它静态分配。
 * 必须 ≥ 实屏几何算出来的值 —— _screen_init 有运行期校验，不符会明确打出来。 */
#define BOARD_CASCADE_BAND_MAX (1400U)

/* ---- 级联总线地址（app_screen_self_addr）----
 * 0 = 主卡，1..0x1F = 从卡。**本期是编译期常量**，即主卡与从卡烧不同固件；
 * 后续期由 W25Qxx 的切分表记录覆盖 —— 同型号板子可以是 2/3/4 卡部署，
 * 那是部署期事实，不可能永久写死在固件里。
 *
 * 注意 3833024 有 DIP1/DIP2 可以直接读出地址（2 bit = 4 码，恰好 1 主 + 3 从），
 * 而 5006048 只有 KEY_TST、没有拨码 —— 所以本参数不能做成"必须靠硬件读"，
 * 否则得给 5006048 改板。 */
#ifndef BOARD_CASCADE_ADDR
#define BOARD_CASCADE_ADDR (0) /* 0 = 主卡 */
#endif

/* ---- 板级组合校验（放在最后：上面几个宏都要已定义）---- */

/* 多卡必须有整屏画布：没有画布就没有"整屏"这个对象，主卡无从切分下发。
 * 这里拦下而不是运行期静默降级 —— 降级的表现是"配了多卡但只有主卡自己那块屏动"。 */
#if BOARD_SCREEN_CANVAS == 0 && (BOARD_CASCADE_COLS > 1 || BOARD_CASCADE_ROWS > 1)
#error "多卡级联需要 BOARD_SCREEN_CANVAS=1（整屏画布）"
#endif

/* "画布池装得下整屏"不在这里判：那要用**实屏几何**（由所选的显示模组驱动决定，
 * 可以用 board.mk 换），board.h 里只有单卡尺寸这个事实，写死会与换屏脱节。
 * _screen_init 有该检查，用的是运行期几何，不符会明确打出来并停用门面。 */
