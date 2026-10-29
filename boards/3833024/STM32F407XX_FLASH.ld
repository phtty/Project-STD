/* 3833024 的 FLASH 布局与共享段脚本的入口 —— **带 IAP bootloader 的版本（默认）**。
 * 构建系统用 -T boards/3833024/STM32F407XX_FLASH.ld 选中本文件：
 *   make（默认 BOOTLOADER=1）、EIDE 目标 "3833024"。
 *
 * 起始地址必须与 boards/3833024/board.h 的 BOARD_VECT_TAB_OFFSET 一致
 * （0x08000000 + 偏移）。两处不一致的表现是任何中断都跳到错误的地方，
 * 且不会有任何编译期报错。
 *
 * 直烧（板上没有升级程序）用**另一个脚本** STM32F407XX_FLASH_direct.ld
 * （0x08000000 / 1024K）—— 由 make 的 BOOTLOADER=0 或 EIDE 的 "3833024-direct"
 * 目标选中，见 board.h 的 bootloader 开关。两个值仍可用 --defsym 应急覆盖。
 */
FLASH_ORIGIN = DEFINED(APP_FLASH_ORIGIN) ? APP_FLASH_ORIGIN : 0x08040000;
FLASH_LENGTH = DEFINED(APP_FLASH_LENGTH) ? APP_FLASH_LENGTH : 768K;

INCLUDE Compiler/sections.ld
