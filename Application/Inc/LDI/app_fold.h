#pragma once

/**
 * @file    app_fold.h
 * @brief   折叠屏模式判定与上下半屏拆分
 *
 * 「折叠屏」是**部署形状**（逻辑屏被切成上下两块等高卡），不是板名。本模块只把
 * 「这块设备是不是折叠屏、以哪种变体工作」这一**纯查询**收拢到一个口径，并把
 * E9 文本按 `\n` 切成上下两段 —— 变体语义（怎么画）由消费方（app_vms_ctrl）决定。
 */

#include <stdint.h>
#include <stdbool.h>

/** @brief 折叠屏工作模式 */
typedef enum {
    APP_FOLD_MODE_FLAT = 0,       /**< 非折叠：整屏一块（3833024 恒走这条） */
    APP_FOLD_MODE_FOLD_E9 = 1,    /**< 折叠 · 变体1：E9 两行分别占上/下半屏 */
    APP_FOLD_MODE_FOLD_E9_EA = 2, /**< 折叠 · 变体2：上半 E9 文本 + 下半 EA 预置图 */
} app_fold_mode_t;

/** @brief 当前折叠屏模式（纯查询、无状态）
 *
 *  判定顺序：
 *    · 几何门禁 `app_screen_fold_count() != 2` → `APP_FOLD_MODE_FLAT`
 *    · 已声明 EA（`app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) != 0xFF`）
 *      → `APP_FOLD_MODE_FOLD_E9_EA`
 *    · 否则 → `APP_FOLD_MODE_FOLD_E9`
 *
 *  几何门禁是保住 3833024 的关键：它不是折叠屏，恒落 FLAT、行为逐字不变。
 *  **判定 EA 不能数 `module_count`** —— 默认配置是 `module_count=2` 而
 *  `modules[1].device_type==0`（空槽，见 app_ldi.c），按数量会把默认配置误判成变体2。
 *  @return 当前模式 */
app_fold_mode_t app_fold_mode(void);

/** @brief 取折叠第 half 块的矩形（薄封装 app_screen_fold_rect）
 *  @param     half 0 = 上半（y 较小），1 = 下半
 *  @param[out] x   接收矩形左上角 X
 *  @param[out] y   接收矩形左上角 Y
 *  @param[out] w   接收矩形宽
 *  @param[out] h   接收矩形高
 *  @return true = 取到（已写输出）；false = 非折叠（一个输出参数都不写） */
bool app_fold_rect(uint8_t half, uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h);

/** @brief 按 `\n` 把文本切成上下两段（只给指针 + 长度，**不修改输入**）
 *
 *  行数 = 1 + `\n` 的个数（**空段也算一行**）：`"A\n"` → 2 行，`""` → 1 行。
 *  `line0` = 第 1 个 `\n` 之前（无 `\n` 时为整段）；`line1` = 第 1 与第 2 个 `\n`
 *  之间（无第 1 个 `\n` 时为空段）。**多于 2 行的部分不由本函数丢弃** —— 它只
 *  返回行数，由调用方决定丢哪些并留日志。
 *  @param     text  输入文本（只读，不被修改）
 *  @param     len   文本字节长度
 *  @param[out] line0 第 1 段起点；可为 nullptr
 *  @param[out] len0  第 1 段字节数；可为 nullptr
 *  @param[out] line1 第 2 段起点；可为 nullptr
 *  @param[out] len1  第 2 段字节数；可为 nullptr
 *  @return 按 `\n` 切出的行数（含空段；无 `\n` 时为 1） */
uint8_t app_fold_split_lines(const char *text, uint16_t len, const char **line0, uint16_t *len0,
                             const char **line1, uint16_t *len1);
