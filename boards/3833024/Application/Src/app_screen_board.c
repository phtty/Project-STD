/**
 * @file    app_screen_board.c
 * @brief   3833024 的级联切分表 —— 本产品 6 模组 / 2 卡不等宽
 *
 * 整屏 144×48（6 个 48×24 模组，2 行 3 列）：
 *
 *   列:      0        1        2
 *   行0:  [从卡]   [主卡]   [主卡]
 *   行1:  [从卡]   [主卡]   [主卡]
 *
 * 主卡带右侧 2×2 = 4 个模组，本地画面 96×48；
 * 从卡带左侧 2×1 = 2 个模组，本地画面 48×48。
 *
 * 等尺寸网格表达不了这种拼法，所以用这张表覆盖 app_screen.c 的默认网格。
 * 主卡格号由 BOARD_CASC_MASTER_CELL 指定（本产品 = 1，即右边那张）。
 */

#include "app_screen.h"

#include "board.h"

static const app_screen_cell_rect_t s_cells[] = {
    /* 格 0：从卡（左侧，1 列 × 2 行模组） */
    {.x = 0U, .y = 0U, .w = 48U, .h = 48U},
    /* 格 1：主卡（右侧，2 列 × 2 行模组） */
    {.x = 48U, .y = 0U, .w = 96U, .h = 48U},
};

static_assert(sizeof(s_cells) / sizeof(s_cells[0]) == 2U, "本产品的切分表固定两张卡");

const app_screen_cell_rect_t *app_screen_board_cells(uint8_t *count)
{
    if (count) *count = (uint8_t)(sizeof(s_cells) / sizeof(s_cells[0]));
    return s_cells;
}
