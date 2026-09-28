/**
 * @file    app_fold_preset_board.c
 * @brief   5006048 的折叠下半屏预置图（绿/红/黄）—— **点阵数据待补**
 *
 * 本板是 1×2 折叠屏（单卡 224×50，整屏 224×100），变体2 的下半屏（224×50）由
 * EA 显示控制选择：01H 绿 / 02H 红 / 03H 黄 → 显示下面三个槽里的对应图。
 *
 * **当前 3 个槽都是空槽（`bitmap=nullptr`、`w=h=0`）**：机制先落、数据后补。
 * app_fold_preset_show() 在绘制前会校验"尺寸必须等于下半屏矩形"，空槽自然被判
 * 不符而拒画 + 打日志，绝不会画出错位的图。
 *
 * TODO（拿到用户提供的真实点阵后，把下面 3 项换成实际数据）：
 *   · 格式：1bpp、行优先、MSB-first、每行 `(w+7)/8` 字节
 *   · 尺寸：**224×50 = 1400 字节**（= 下半屏 = BOARD_CASC_BAND_MAX）
 *   · 索引顺序不可换：0 = 绿(Color=01H)、1 = 红(02H)、2 = 黄(03H)
 *
 * 两块板都需要这个符号（共享的 app_fold.c 无条件引用它，见 AGENTS.md §6 坑 6）；
 * 3833024 不折叠、恒走 FLAT，它的三个槽保持空即可。
 */

#include "app_fold.h"

const app_fold_preset_t g_board_fold_presets[3] = {
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [0] 绿（TODO：224×50、1400B） */
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [1] 红（TODO：224×50、1400B） */
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [2] 黄（TODO：224×50、1400B） */
};
