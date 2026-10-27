/* 3833024 的 FLASH 布局与共享段脚本的入口。
 * 构建系统用 -T boards/3833024/STM32F407XX_FLASH.ld 选中本文件。
 *
 * 起始地址必须与 boards/3833024/board.h 的 BOARD_VECT_TAB_OFFSET 一致
 * （0x08000000 + 偏移）。两处不一致的表现是任何中断都跳到错误的地方，
 * 且不会有任何编译期报错。
 *
 * 两个值可由构建系统用 --defsym 覆盖（见 board.h 的 bootloader 开关）：
 *   带 bootloader（默认）：0x08040000 / 768K
 *   直烧（BOOTLOADER=0） ：0x08000000 / 1024K
 */
FLASH_ORIGIN = DEFINED(APP_FLASH_ORIGIN) ? APP_FLASH_ORIGIN : 0x08040000;
FLASH_LENGTH = DEFINED(APP_FLASH_LENGTH) ? APP_FLASH_LENGTH : 768K;

INCLUDE Compiler/sections.ld
