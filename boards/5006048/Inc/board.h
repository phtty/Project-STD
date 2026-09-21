/**
 * @file    board.h
 * @brief   5006048 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR)/Inc 选中（Makefile 里排在第一位的头搜索路径），
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
