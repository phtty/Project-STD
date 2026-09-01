/**
 * @file    app_default_display.h
 * @brief   注册制默认显示界面 — 协议模块可选覆盖，默认欢迎画面兜底
 *
 * 协议模块可在 sw_initcall（sw_board_init 内执行，早于 init_task 中
 * app_default_display 调用）注册自己的默认显示回调；未注册时回退默认
 * 欢迎画面。单槽设计：首个注册生效（多协议共存时归属由 initcall
 * 字母序决定；量产地区协议互斥，无实际竞争）。
 */

#pragma once

typedef void (*app_default_display_fn_t)(void);

/** @brief 注册默认显示回调（首个注册生效，后续注册忽略） */
void app_default_display_register(app_default_display_fn_t fn);

/** @brief 渲染默认显示界面：有注册回调则调用之，否则渲染默认欢迎画面 */
void app_default_display(void);

/**
 * @brief 恢复默认显示界面：有注册回调则调用之，否则清屏兜底。
 * @note  供协议模块在显示覆盖画面（如 CQ 心跳故障屏）后恢复默认画面调用；
 *        与 app_default_display 的区别：无注册回调时清屏而非渲染欢迎画面。
 */
void app_default_display_show(void);