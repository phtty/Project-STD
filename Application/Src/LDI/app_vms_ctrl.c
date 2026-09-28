/**
 * @file    app_vms_ctrl.c
 * @brief   VMS 情报板控制实现：文本显示与定时清屏
 */

#include "app_vms_ctrl.h"
#include "app_screen.h"
#include "app_fold.h"

#include <stdio.h>
#include "string.h"

#include "app_render.h"
#include "app_light_sensor.h"

/* ---- VMS 定时清屏 ---- */
static uint32_t s_vms_clear_tick; /* 清屏时刻 (RTOS tick) */
static bool     s_vms_timer_active; /* 定时器是否激活 */

static void _vms_clear_screen(void)
{
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = DEV_DISPLAY_COLOR_BLACK,
    });
    /* **这里不再存**：清屏本身不该落盘（定时显示到点也是走这条），
       而"永久显示"的那次清屏由紧随其后的文字帧带 `.persist` 一起覆盖 ——
       落盘发生在**落屏之后**（app_screen 取走请求），一次写、内容还是最终那一帧。
       原来这里是"清屏立刻 app_render_save()"，存下去的是清屏**之前**的上一帧。 */
}

/** @brief 清 E9 的内容区（变体2 只清上半；其余整屏）
 *
 *  两个 E9 清屏调用点共用：显示控制先清一次、定时到点再清一次。变体2 的下半是
 *  EA 预置图，**任何 E9 的清屏都不得覆盖它** —— 否则一条到点清屏会顺手把用户设的
 *  雨棚预置图抹掉。非变体2 时与 _vms_clear_screen 逐字一致。 */
static void _vms_clear_content(void)
{
    if (app_fold_mode() == APP_FOLD_MODE_FOLD_E9_EA) {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        if (app_fold_rect(0, &x, &y, &w, &h)) {
            app_render(&(app_render_cfg_t){
                .type  = APP_RENDER_TYPE_FILL,
                .x     = x,
                .y     = y,
                .w     = w,
                .h     = h,
                .color = DEV_DISPLAY_COLOR_BLACK,
            });
            return;
        }
    }
    _vms_clear_screen();
}

void app_vms_timer_poll(void)
{
    if (!s_vms_timer_active) return;

    if (osKernelGetTickCount() >= s_vms_clear_tick) {
        _vms_clear_content();
        s_vms_timer_active = false;
    }
}

/* ================================================================
 *  查表映射 — LDI 协议编号 → 渲染引擎枚举
 * ================================================================ */

/** LDI 字体颜色 → dev_display_color_t (01H=绿, 02H=红, 03H=黄, 其他=黑) */
static const dev_display_color_t s_color_map[] = {
    [1] = DEV_DISPLAY_COLOR_GREEN,
    [2] = DEV_DISPLAY_COLOR_RED,
    [3] = DEV_DISPLAY_COLOR_YELLOW,
};

/** LDI 对齐方式 format → app_render_align_t (01H=居中, 02H=左, 03H=右) */
static const app_render_align_t s_align_map[] = {
    [1] = APP_RENDER_ALIGN_CENTER,
    [2] = APP_RENDER_ALIGN_LEFT_UP,
    [3] = APP_RENDER_ALIGN_RIGHT_DOWN,
};

/** LDI 字号 font_size → app_font_size_t (00H=自适应, 01H=16, 02H=24, 03H=32) */
static const app_font_size_t s_font_size_map[] = {
    [0] = APP_FONT_SIZE_SELF_ADAPT,
    [1] = APP_FONT_SIZE_16,
    [2] = APP_FONT_SIZE_24,
    [3] = APP_FONT_SIZE_32,
};

/** LDI 清屏颜色 clear_type → dev_display_color_t (00H=黑, 01H=红, 02H=绿, 03H=黄) */
static const dev_display_color_t s_clear_color_map[] = {
    [0] = DEV_DISPLAY_COLOR_BLACK,
    [1] = DEV_DISPLAY_COLOR_RED,
    [2] = DEV_DISPLAY_COLOR_GREEN,
    [3] = DEV_DISPLAY_COLOR_YELLOW,
};

/* ---- 带边界检查的查表辅助宏 ---- */
#define MAP(table, idx, fallback) \
    ((idx) < (sizeof(table) / sizeof((table)[0])) ? (table)[(idx)] : (fallback))

/* ================================================================
 *  显示控制 (func_type = 0x01)
 * ================================================================ */

static void _vms_display_ctrl(app_ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    /* 文字显示: 恢复自动亮度跟随 */
    app_light_sensor_resume();

    dev_display_color_t color     = MAP(s_color_map, ctx->font_color, DEV_DISPLAY_COLOR_BLACK);
    app_render_align_t         h_align   = MAP(s_align_map, ctx->format, APP_RENDER_ALIGN_CENTER);
    app_font_size_t     font_size = MAP(s_font_size_map, ctx->font_size, APP_FONT_SIZE_16);

    /* **逻辑屏**尺寸，不是本卡那块屏 —— 级联时整屏比本卡屏大，用本卡的
       screen_rows/cols 算布局会让内容整块落到别的卡那半边去。 */
    uint16_t screen_w = app_screen_rows();
    uint16_t screen_h = app_screen_cols();

    /* ---- 计算行布局 ---- */
    uint16_t render_y = 0;
    uint16_t render_h = screen_h;
    app_render_align_t  v_align  = APP_RENDER_ALIGN_CENTER;

    /* font_line > 0: 将屏幕按字号划分为若干行，文字显示在指定行；
       font_line == 0: 上下居中 (默认) */
    if (ctx->font_line > 0) {
        /* 自适应字号时以 16 为基准计算行高 */
        uint16_t line_height = (font_size == APP_FONT_SIZE_SELF_ADAPT) ? 16U : (uint16_t)font_size;
        uint16_t total_lines = screen_h / line_height;

        if (total_lines > 0 && ctx->font_line <= total_lines) {
            render_y = (ctx->font_line - 1) * line_height;
            render_h = line_height;
            v_align  = APP_RENDER_ALIGN_LEFT_UP; /* 单行内不居中 */

            /* 强制使用具体字号，否则渲染器会自适应缩放 */
            if (font_size == APP_FONT_SIZE_SELF_ADAPT) font_size = APP_FONT_SIZE_16;
        }
        /* font_line 超出范围 → 回退到居中模式 */
    }

    /* ---- 构建渲染风格 ---- */
    app_render_style_t style = {
        .h_align   = h_align,
        .v_align   = v_align,
        .word_wrap = false,
    };

    /* ---- LDI 协议 '_' → 换行 ---- */
    for (uint16_t i = 0; i < text_len; i++)
        if (ctx->text[i] == '_')
            ctx->text[i] = '\n';

    /* ---- 清屏（只清一次；变体2 只清上半，下半留给 EA 预置图）---- */
    const app_fold_mode_t fold_mode = app_fold_mode();
    _vms_clear_content();

    if (fold_mode == APP_FOLD_MODE_FLAT) {
        /* 非折叠：单次渲染，区域 = 整块逻辑屏 —— 与引入折叠前逐字一致（3833024 走这条）。 */
        app_render(&(app_render_cfg_t){
            .type      = APP_RENDER_TYPE_TEXT,
            .x         = 0,
            .y         = render_y,
            .w         = screen_w,
            .h         = render_h,
            .color     = color,
            .text      = (char *)ctx->text,
            .len       = text_len,
            .style     = &style,
            .font_size = font_size,
            .font_type = APP_FONT_TYPE_HT,
            .text_enc  = APP_FONT_ENC_GBK,
            /* 永久显示（keep_time==0）才落盘；定时显示是临时内容，不该占 flash。
               落盘由 app_screen 在**落屏之后**做 —— 原来这里紧跟的 app_render_save()
               在画布还没落屏时就存，存下去是上一帧。 */
            .persist   = (ctx->keep_time == 0),
        });
    } else {
        /* 折叠屏变体：E9 的行严格限在半屏内（区域契约保证不跨缝）。
           变体1（FOLD_E9）：两行分别占上/下半屏。
           变体2（FOLD_E9_EA）：**只写上半屏** —— 下半是 EA 预置图，E9 不得覆盖。 */
        const char *line0 = nullptr;
        const char *line1 = nullptr;
        uint16_t    len0  = 0;
        uint16_t    len1  = 0;
        const uint8_t line_cnt =
            app_fold_split_lines((const char *)ctx->text, text_len, &line0, &len0, &line1, &len1);

        const bool is_v2 = (fold_mode == APP_FOLD_MODE_FOLD_E9_EA);

        /* 折叠屏整体单色：记下最近一次颜色，EA 预置图取同一色 */
        app_fold_note_color((uint8_t)color);

        /* 折叠模式**忽略 font_line**：它的原语义是"把内容放到屏幕第 N 行"，在上下半屏里
           会把内容放到跨缝的位置。样式开 word_wrap + prefer_one_line（优先单行最大字号，
           最小字号才换行）。keep_time/persist 语义不变（持久化粒度 = 半屏 = 本卡矩形）。 */
        app_render_style_t fold_style = {
            .h_align         = h_align,
            .v_align         = APP_RENDER_ALIGN_CENTER,
            .word_wrap       = true,
            .prefer_one_line = true,
        };
        const bool fold_persist = (ctx->keep_time == 0);

        uint16_t fx = 0, fy = 0, fw = 0, fh = 0;

        /* 上半屏：第 1 行（1 行时只画这里，下半保持清空 / 保持 EA 预置图） */
        if (app_fold_rect(0, &fx, &fy, &fw, &fh)) {
            app_render(&(app_render_cfg_t){
                .type      = APP_RENDER_TYPE_TEXT,
                .x         = fx,
                .y         = fy,
                .w         = fw,
                .h         = fh,
                .color     = color,
                .text      = (char *)line0,
                .len       = len0,
                .style     = &fold_style,
                .font_size = font_size,
                .font_type = APP_FONT_TYPE_HT,
                .text_enc  = APP_FONT_ENC_GBK,
                .persist   = fold_persist,
            });
        }

        /* 下半屏：第 2 行 —— **变体2 不写**（那一半是 EA 预置图） */
        if (!is_v2 && line_cnt >= 2 && app_fold_rect(1, &fx, &fy, &fw, &fh)) {
            app_render(&(app_render_cfg_t){
                .type      = APP_RENDER_TYPE_TEXT,
                .x         = fx,
                .y         = fy,
                .w         = fw,
                .h         = fh,
                .color     = color,
                .text      = (char *)line1,
                .len       = len1,
                .style     = &fold_style,
                .font_size = font_size,
                .font_type = APP_FONT_TYPE_HT,
                .text_enc  = APP_FONT_ENC_GBK,
                .persist   = fold_persist,
            });
        }

        /* 放不下的行丢弃（只留日志，不静默）：变体1 只有上下两半，变体2 下半归 EA。 */
        if (is_v2) {
            if (line_cnt > 1)
                printf("[ldi/vms] 变体2：下半为 EA 预置图，E9 只显示上半 1 行、丢弃 %u 行\n",
                       (unsigned)(line_cnt - 1));
        } else if (line_cnt > 2) {
            printf("[ldi/vms] 折叠屏只有上下两半：文本 %u 行，仅显示前 2 行、丢弃 %u 行\n",
                   (unsigned)line_cnt, (unsigned)(line_cnt - 2));
        }
    }

    /* ---- 持久化策略 ---- */
    if (ctx->keep_time == 0) {
        s_vms_timer_active = false; /* 落盘请求已随渲染提交，见上一段 */
    } else {
        /* 定时显示：keep_time 秒后自动清屏 */
        s_vms_clear_tick   = osKernelGetTickCount() + (uint32_t)ctx->keep_time * 1000U;
        s_vms_timer_active = true;
    }
}

/* ================================================================
 *  清屏控制 (func_type = 0x02)
 * ================================================================ */

static void _vms_clean_ctrl(app_ldi_ctrl_vms_t *ctx)
{
    /* 全屏点亮/清屏: 停止自动亮度跟随, 固定最大亮度 (非黑屏) */
    app_light_sensor_set_fixed(7);

    dev_display_color_t color = MAP(s_clear_color_map, ctx->clear_type, DEV_DISPLAY_COLOR_BLACK);

    /* 变体2：下半是 EA 预置图，E9 的 02H 清屏只清上半（同显示控制的口径）。 */
    if (app_fold_mode() == APP_FOLD_MODE_FOLD_E9_EA) {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        if (app_fold_rect(0, &x, &y, &w, &h)) {
            app_render(&(app_render_cfg_t){
                .type  = APP_RENDER_TYPE_FILL,
                .x     = x,
                .y     = y,
                .w     = w,
                .h     = h,
                .color = color,
            });
        }
    } else {
        app_render(&(app_render_cfg_t){
            .type  = APP_RENDER_TYPE_FILL,
            .x     = 0,
            .y     = 0,
            .w     = 0,
            .h     = 0,
            .color = color,
        });
    }

    /* 主动清屏时取消定时器 */
    s_vms_timer_active = false;
}

/* ---- 公共入口 ---- */
void app_vms_ctrl(app_ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    if (ctx->device_func_type == 0x01)
        _vms_display_ctrl(ctx, text_len);
    else if (ctx->device_func_type == 0x02)
        _vms_clean_ctrl(ctx);
}
