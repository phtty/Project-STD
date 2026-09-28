/**
 * @file    app_fold.c
 * @brief   折叠屏模式判定与上下半屏拆分（纯查询、无状态）
 */

#include "app_fold.h"

#include "app_screen.h"
#include "app_ldi.h"

app_fold_mode_t app_fold_mode(void)
{
    /* 几何门禁放第一位：3833024 是 1×1 单卡，fold_count 恒 1 —— 它必须逐字走 FLAT，
       否则一条与折叠无关的 E9 帧会试图往"上半/下半"里塞内容。 */
    if (app_screen_fold_count() != 2U) return APP_FOLD_MODE_FLAT;

    /* 变体2 的判据是"EA 被声明为一块模块"，**不是** module_count：
       默认配置 module_count==2 而 modules[1].device_type==0（空槽），
       按数量判定会把默认配置误判成变体2。 */
    if (app_ldi_get_device_idx(APP_LDI_DEVICE_CANOPY_LIGHT) != 0xFF)
        return APP_FOLD_MODE_FOLD_E9_EA;

    return APP_FOLD_MODE_FOLD_E9;
}

bool app_fold_rect(uint8_t half, uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    return app_screen_fold_rect(half, x, y, w, h);
}

uint8_t app_fold_split_lines(const char *text, uint16_t len, const char **line0, uint16_t *len0,
                             const char **line1, uint16_t *len1)
{
    /* 行数 = 1 + '\n' 个数（空段也算一行）。 */
    uint8_t lines = 1;
    for (uint16_t i = 0; i < len; i++)
        if (text[i] == '\n') lines++;

    /* 第 1 段：起点 0，止于第 1 个 '\n'（无则到 len）。 */
    uint16_t end0 = len;
    for (uint16_t i = 0; i < len; i++)
        if (text[i] == '\n') {
            end0 = i;
            break;
        }
    if (line0) *line0 = text;
    if (len0) *len0 = end0;

    if (end0 == len) { /* 没有 '\n'：第 2 段为空段 */
        if (line1) *line1 = text + len;
        if (len1) *len1 = 0;
        return lines;
    }

    /* 第 2 段：第 1 个 '\n' 之后，止于第 2 个 '\n'（无则到 len）。 */
    const char *begin1 = text + end0 + 1;
    uint16_t    end1   = len;
    for (uint16_t i = (uint16_t)(end0 + 1); i < len; i++)
        if (text[i] == '\n') {
            end1 = i;
            break;
        }
    if (line1) *line1 = begin1;
    if (len1) *len1 = (uint16_t)(end1 - end0 - 1);

    return lines;
}
