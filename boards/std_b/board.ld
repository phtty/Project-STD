/* std_b 的 FLASH 布局：无 IAP bootloader，直接烧到 0x08000000，整片 1024K 可用。
 * 与 boards/std_b/Inc/board.h 的 BOARD_VECT_TAB_OFFSET（0）必须一致。 */
FLASH_ORIGIN = 0x08000000;
FLASH_LENGTH = 1024K;
