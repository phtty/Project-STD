/* std_a 的 FLASH 布局：前 4 个扇区（64K）留给 IAP bootloader，主固件从 0x08040000 起。
 * 与 boards/std_a/Core/Src/main.c 的 VECT_TAB_OFFSET 必须一致。 */
FLASH_ORIGIN = 0x08040000;
FLASH_LENGTH = 768K;
