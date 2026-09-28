/**
 * @file    app_fold.c
 * @brief   折叠屏模式判定、上下半屏拆分与变体2 下半屏预置图
 */

#include "app_fold.h"

#include <stdio.h>

#include "app_screen.h"
#include "app_ldi.h"
#include "app_render.h"
#include "dev_display.h" /* dev_display_frame_begin/end：把"清+画"两笔当成一帧 */

/* ---- 折叠屏最近一次显示颜色 ----
 *
 *  "折叠屏整体单色 = 最近一条命令的颜色"这个口径的落点：E9 的折叠路径在渲染时
 *  `app_fold_note_color()` 更新，EA 取图时按它渲染。从未有过则为 BOARD_SCREEN_COLOR
 *  （与"单卡默认色"一致，见 board.h）。跨模块只经 API 更新，不导出可变全局。 */
static uint8_t s_last_color = (uint8_t)BOARD_SCREEN_COLOR;

void app_fold_note_color(uint8_t color)
{
    s_last_color = color;
}

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

/* ================================================================
 *  变体2 · 下半屏预置图 / 清除
 * ================================================================ */

bool app_fold_preset_show(uint8_t color)
{
    if (color < 1U || color > 3U) {
        printf("[fold] EA 颜色 0x%02X 无对应预置图（仅 01H~03H），拒画\n", (unsigned)color);
        return false;
    }

    uint16_t x = 0, y = 0, w = 0, h = 0;
    if (!app_fold_rect(1, &x, &y, &w, &h)) return false; /* 非折叠：没有下半屏 */

    const app_fold_preset_t *p = &g_board_fold_presets[color - 1U];
    if (p->bitmap == nullptr) {
        printf("[fold] EA 颜色 %u 的预置图为空槽（点阵数据待补），拒画\n", (unsigned)color);
        return false;
    }
    /* 尺寸必须等于下半屏：不符说明数据是按别的模组/别的部署切的，画上去会错位 —— 拒画。 */
    if (p->w != w || p->h != h) {
        printf("[fold] EA 预置图 %ux%u != 下半屏 %ux%u，拒画\n", (unsigned)p->w, (unsigned)p->h,
               (unsigned)w, (unsigned)h);
        return false;
    }

    /* "清 + 画"两笔**当成一帧**输出（与 app_screen_commit_bitmap / app_factory_test.c
       同款，见 dev_display.h 的 dev_display_frame_begin 说明）：
       · 两笔之间不让 `_scan_task` 跑 prepare —— 直写实屏路径否则可能在 FILL 之后、
         BITMAP 之前抢跑一次 prepare，屏上闪一帧"半清空"中间态（下半屏只剩底色、
         位图还没画上）。
       · 画布路径（多卡主卡）整段都不碰实屏缓冲，frame 标记不参与：`frame_end`
         不会白跑一次 prepare（`frame_touched` 记着），画布由 settle/轮次那一侧提交上屏。
       · **校验全部通过之后才 begin**：上面任何一条校验失败都已直接 return，一个渲染
         都不发、也不 begin —— 不留未配对的 begin 把脏标记永久压住。
       两笔的顺序与语义：必须先清下半屏、再画位图。位图语义是 bit=1 才写（上色）、
       bit=0 不动（见 app_screen_canvas.c 的 _sink_bitmap 与 dev_display_draw_bitmap），
       两张预置图切换时新图 bit=0 的像素会保留旧图 —— 残影。E9 折叠路径同样是
       "先清半屏再画"。半屏全黑填充正是画布颜色账里的"新一帧"（见
       docs/架构说明.md §7.4）。

       **清屏这一笔不落盘**（.persist 默认 false，见 app_render.h 的 persist 说明）：
       整帧的最终状态是图 —— 由下面 BITMAP 那一笔请求落盘。此处清屏只是"画图前的
       过渡笔"，本身不是要留存的内容帧；清屏落盘会让掉电后复活一张黑屏。 */
    dev_display_frame_begin(dev_display_get());

    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = x,
        .y     = y,
        .w     = w,
        .h     = h,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });

    /* 画图这一笔才落盘（掉电恢复）：雨棚状态是产品状态，掉电重来要恢复它 ——
       **清屏不落盘、画图落盘**，整帧最终状态是图。
       粒度仍是"半屏（本卡矩形）"，记录格式未变（见 docs/架构说明.md §7.4）。 */
    app_render(&(app_render_cfg_t){
        .type    = APP_RENDER_TYPE_BITMAP,
        .x       = x,
        .y       = y,
        .w       = w,
        .h       = h,
        .color   = (dev_display_color_t)s_last_color,
        .bitmap  = p->bitmap,
        .persist = true,
    });

    dev_display_frame_end(dev_display_get());
    return true;
}

bool app_fold_lower_clear(void)
{
    uint16_t x = 0, y = 0, w = 0, h = 0;
    if (!app_fold_rect(1, &x, &y, &w, &h)) return false; /* 非折叠：没有下半屏 */

    /* **清屏不落盘**（.persist 默认 false，不给默认、见 app_render.h 的 persist 说明）：
       清屏是**当场生效的瞬时状态**，靠上位机后续命令重设，不是要留存的内容帧。
       持久化策略排除清屏，是为了保住"上电有内容可判屏"这一现场手段 —— 交通屏掉电
       重启后必须是**画面**（哪怕是旧的/错的），现场才能一眼判断"屏体本身是否正常"；
       若把清屏存下去，重启即黑屏，就失去这个判屏依据。旧图在掉电后被"复活"是可接受
       且预期的，内容由上位机重新下发纠正。粒度仍是"半屏（本卡矩形）"，记录格式未变。

       **单笔填充不包 frame_begin/end**：只有一笔，没有"两笔之间的中间态"可压；包了
       只是把置脏推迟到 `_end`，上屏结果与直接画没有任何区别（见 dev_display.h 的
       dev_display_frame_begin 说明）—— 无收益，还多一处必须成对的约束。预置图的
       "清 + 画"两笔才需要。 */
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = x,
        .y     = y,
        .w     = w,
        .h     = h,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    return true;
}
