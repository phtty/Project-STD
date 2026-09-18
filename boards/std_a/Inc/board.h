/**
 * @file    board.h
 * @brief   std_a 板级配置常量
 *
 * 共享代码需要知道"这块板把哪个外设派了什么用"时，从这里取，不要在共享文件里
 * 写死某个具体的 TIMx/USARTx——那是板级事实，换板就变。
 *
 * 本文件由 -I $(BOARD_DIR)/Inc 选中（Makefile 里排在第一位的头搜索路径），
 * 因此共享代码统一写 #include "board.h" 即可，无需条件编译。
 */

#pragma once

/* ---- 显示子系统的定时器角色 ----
 * std_a：TIM3 出行同步中断（行扫描节拍），TIM4 出 8 级亮度 PWM。
 * OE/LAT 原子窗口要屏蔽的是 PWM 那个中断——PWM 在窗口中间跳变会把消隐时序打断。 */
#define BOARD_DISPLAY_SCAN_TIM PL_TIM3
#define BOARD_DISPLAY_PWM_TIM  PL_TIM4
