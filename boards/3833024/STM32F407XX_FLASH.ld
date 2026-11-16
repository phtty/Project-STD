/* 2300001735 直烧产品：无 IAP bootloader、无升级，固件从 0x08000000 起铺满 1MB。
 * 原 3833024 的 0x08040000/768K 布局留给带 IAP 的通用固件（wyh 主线）。
 *
 * 起始地址必须与 boards/3833024/board.h 的 BOARD_VECT_TAB_OFFSET（本产品为 0）
 * 一致，且 main.c 的 SCB->VTOR 直接取 0x08000000。三处不一致的表现是任何中断
 * 都跳到错误的地方，且不会有任何编译期报错。 */
FLASH_ORIGIN = 0x08000000;
FLASH_LENGTH = 1024K;

INCLUDE Compiler/sections.ld
