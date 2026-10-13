/* 5006048 的 FLASH 布局与共享段脚本的入口。
 * 构建系统用 -T boards/5006048/STM32F407XX_FLASH.ld 选中本文件。
 *
 * 起始地址必须与 boards/5006048/board.h 的 BOARD_VECT_TAB_OFFSET 一致
 * （0x08000000 + 偏移）。两处不一致的表现是任何中断都跳到错误的地方，
 * 且不会有任何编译期报错。 */
FLASH_ORIGIN = 0x08000000;
FLASH_LENGTH = 1024K;

INCLUDE Compiler/sections.ld
