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

/* ---- 向量表偏移 ----
 * 必须与 boards/5006048/board.ld 的 FLASH_ORIGIN 保持一致：5006048 直烧 0x08000000，
 * 偏移 0；带 IAP bootloader 的板子则是 0x40000。两处不一致的表现是任何中断都
 * 跳到错误的地方（bootloader 的向量表或空白区），且不会有编译期报错。 */
#define BOARD_VECT_TAB_OFFSET 0x00000000UL

/* ---- 显示子系统的定时器角色 ----
 * 5006048：TIM2 出行同步中断（行扫描节拍），TIM3 出 8 级亮度 PWM。
 * 注意与 3833024 不同（那边是 TIM3/TIM4）——5006048 的 CubeMX 配置里没有 TIM4。
 * OE/LAT 原子窗口要屏蔽的是 PWM 那个中断：PWM 在窗口中间跳变会打断消隐时序。 */
#define BOARD_DISPLAY_SCAN_TIM PL_TIM2
#define BOARD_DISPLAY_PWM_TIM  PL_TIM3
