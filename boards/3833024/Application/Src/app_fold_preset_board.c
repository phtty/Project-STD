/**
 * @file    app_fold_preset_board.c
 * @brief   3833024 的折叠预置图表 —— 本板不折叠，仅提供可链接的空槽
 *
 * 3833024 是 1×1 单卡、几何门禁恒判 FLAT，永远不会走到变体2 的预置图路径。
 * 但共享的 app_fold.c **无条件**引用 `g_board_fold_presets`，本板必须有这个符号，
 * 否则链接报"被引用却未定义"（见 AGENTS.md §6 坑 6）。三个槽保持空槽即可。
 *
 * 若将来本板改成折叠部署，按 5006048/Application/Src/app_fold_preset_board.c 的
 * TODO 说明填入真实点阵（索引 0/1/2 = 绿/红/黄）。
 */

#include "app_fold.h"

const app_fold_preset_t g_board_fold_presets[3] = {
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [0] 绿（本板不用） */
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [1] 红（本板不用） */
    {.w = 0, .h = 0, .bitmap = nullptr}, /* [2] 黄（本板不用） */
};
