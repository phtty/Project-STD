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
 * 必须与 Src/font_lib_board.c 的 g_board_font.total_bytes 一致。不同步的后果
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
/* 本卡颜色（display_color_t）。级联后由切分表逐卡给，这里只是单卡时的默认值。 */
#define BOARD_SCREEN_COLOR      (2) /* COLOR_GREEN */

/* ---- 级联总线地址（app_screen_self_addr）----
 * 0 = 主卡，1..0x1F = 从卡。**本期是编译期常量**，即主卡与从卡烧不同固件；
 * 后续期由 W25Qxx 的切分表记录覆盖 —— 同型号板子可以是 2/3/4 卡部署，
 * 那是部署期事实，不可能永久写死在固件里。
 *
 * 注意 3833024 有 DIP1/DIP2 可以直接读出地址（2 bit = 4 码，恰好 1 主 + 3 从），
 * 而 5006048 只有 KEY_TST、没有拨码 —— 所以本参数不能做成"必须靠硬件读"，
 * 否则得给 5006048 改板。 */
#ifndef BOARD_CASCADE_ADDR
#define BOARD_CASCADE_ADDR      (0) /* 0 = 主卡 */
#endif
