/**
 * @file    board.h
 * @brief   3833024 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR)/Inc 选中（Makefile 里排在第一位的头搜索路径），
 * 因此共享代码统一写 #include "board.h" 即可，无需条件编译。
 */

#pragma once

/* ---- 向量表偏移 ----
 * 必须与 boards/3833024/board.ld 的 FLASH_ORIGIN 保持一致：3833024 带 IAP bootloader，
 * 主固件从 0x08040000 起，偏移 0x40000；直烧的板子是 0。两处不一致的表现是任何
 * 中断都跳到错误的地方，且不会有编译期报错。 */
#define BOARD_VECT_TAB_OFFSET 0x00040000UL
