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

/* ================================================================
 *  变体2 · 下半屏预置图（EA 显示控制的数据面）
 *
 *  EA 的 `Color` = 01H 绿 / 02H 红 / 03H 黄 → 显示**下半屏**对应的预置图；
 *  `Color` = 00H（产品扩展值）→ 清除下半屏。图片数据是**板级事实**（用户提供），
 *  本模块只按"索引 = Color-1"取图并校验尺寸，不关心点阵内容。
 * ================================================================ */

/** @brief 折叠下半屏的预置图描述符（1bpp、行优先、MSB-first） */
typedef struct {
    uint16_t w;            /**< 位图宽（像素），绘制前须等于**下半屏宽** */
    uint16_t h;            /**< 位图高（像素），绘制前须等于**下半屏高** */
    const uint8_t *bitmap; /**< 1bpp 位图首地址（每行 (w+7)/8 字节、MSB-first）；
                            *   `nullptr` = 空槽（数据待补），绘制前被拒 */
} app_fold_preset_t;

/** @brief 板级折叠预置图表 —— **下标 0/1/2 = 绿/红/黄**（与 EA 的 `Color-1` 对应）
 *
 *  唯一实例由 `boards/<板>/Application/Src/app_fold_preset_board.c` 提供，**两块板都要**
 *  （共享的 app_fold.c 无条件引用它，缺了会链接不过，见 AGENTS.md §6 坑 6）。
 *  5006048 的三个槽当前是空槽（真实点阵待用户提供），空槽与尺寸不符都在绘制前拒画。 */
extern const app_fold_preset_t g_board_fold_presets[3];

/** @brief 记下"折叠屏最近一次显示的颜色"（E9 的折叠路径在渲染时调用）
 *
 *  折叠屏整体单色 —— 变体2 的 E9 文本与 EA 预置图应当是**同一条命令给的同一个颜色**，
 *  所以把"最近一次颜色"收成折叠模块的状态，EA 取图时按它渲染。跨模块更新走本 API，
 *  不导出可变全局（见 `docs/命名约定.md` §4.7）。
 *  @param color 本次显示用的颜色（`dev_display_color_t`） */
void app_fold_note_color(uint8_t color);

/** @brief 在下半屏显示 `color` 对应的预置图（01H=绿 / 02H=红 / 03H=黄）
 *
 *  颜色取"折叠屏最近一次用过的颜色"（`app_fold_note_color` 记的），从未有过则为
 *  `BOARD_SCREEN_COLOR`。绘制前**校验预置图尺寸必须等于下半屏矩形**，不符（含空槽）
 *  即拒画 + 日志并返回 false —— 空槽自然走这条路，不会画出错位的图。
 *  @param color 预置图索引 + 1（01H~03H）
 *  @return true = 已发起绘制；false = 颜色非法 / 非折叠 / 空槽 / 尺寸不符（均不画） */
bool app_fold_preset_show(uint8_t color);

/** @brief 清除下半屏（全黑填充，**不动**上半屏）
 *
 *  变体2 的 EA `Color=00H` 走这条。清屏区域是下半屏矩形，供画布把"半屏全黑"
 *  也算作新一帧（颜色账复位）。
 *  @return true = 已发起清屏；false = 非折叠（不动作） */
bool app_fold_lower_clear(void);
