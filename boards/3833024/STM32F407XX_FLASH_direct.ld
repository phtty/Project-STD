/* 3833024 的 FLASH 布局 —— **直烧版**（板上没有 IAP bootloader）。
 *
 * 由构建系统用 -T 选中：
 *   make BOOTLOADER=0                       （Makefile 自动换脚本并加 -D）
 *   EIDE：目标下拉里选 "3833024-direct"      （定义与脚本都在该目标里配好）
 *
 * 与 STM32F407XX_FLASH.ld 的**唯一区别**是起始地址与长度：应用从 0x08000000
 * 起铺满整片，向量表偏移 0（board.h 的 BOARD_VECT_TAB_OFFSET）。
 * 两处不一致的表现是任何中断都跳到错误的地方，且不会有任何编译期报错。
 *
 * 直烧板把 0x08000000~0x0803FFFF 全交给应用，**包括 0x08004000 的记录扇区** ——
 * 那里现在是应用自己的代码，所以 BOARD_HAS_IAP_RECORD 必须为 0（由
 * BOARD_HAS_IAP_BOOTLOADER=0 派生），app_iap_cfg.c 的静态断言会卡住配错的情况。
 */
FLASH_ORIGIN = 0x08000000;
FLASH_LENGTH = 1024K;

INCLUDE Compiler/sections.ld
