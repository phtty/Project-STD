/**
 * @file    app_screen_canvas.c
 * @brief   整屏逻辑画布实现 —— 1bpp 画布、渲染目标、抽带、落屏与持久化
 *
 * 由 app_screen.c 拆出：几何/身份/亮度/卡状态留在门面（app_screen.c），
 * 画布那一簇（缓冲、sink、抽带、静默期提交、持久化）收在这里。
 * 开关 BOARD_SCREEN_CANVAS=0 时整份实现不进构建，只留两个桩符号。
 */

#include "app_screen.h"
