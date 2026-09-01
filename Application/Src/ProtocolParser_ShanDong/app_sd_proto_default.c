/**
 * @file    app_sd_proto_default.c
 * @brief   山东协议上电效果 — 注册默认显示界面
 *
 * 协议文档 §0：上电后点阵全屏显示「山东省 高速公路 欢迎您」。
 * 经 app_default_display_register 注册为默认显示回调（sw_app_initcall
 * 早于 init_task 中 app_default_display 调用），覆盖默认欢迎画面。
 */

#include <stdint.h>

#include "app_default_display.h"
#include "app_render.h"
#include "dev_display.h"
#include "initcall.h"

/* 「山东省 高速公路 欢迎您」（协议文档 §0 上电显示原文，UTF-8 字面量） */
static const uint8_t s_sd_default_text[] = "山东省\n高速公路\n欢迎您";

/** @brief 清屏后居中渲染「山东省 高速公路 欢迎您」（FONT_16 居中黄字）。 */
static void _sd_default_show(void)
{
    dev_display_t *d = dev_display_get();
    if (!d)
        return;

    dev_display_fill(d, 0, 0, d->screen_rows, d->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(d);

    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = 0,
        .w     = d->screen_rows,
        .h     = d->screen_cols,
        .style = &(render_style_t){
            .h_align   = ALIGN_CENTER,
            .v_align   = ALIGN_CENTER,
            .word_wrap = false,
        },
        .color     = COLOR_YELLOW,
        .text      = (const char *)s_sd_default_text,
        .len       = sizeof(s_sd_default_text) - 1U,
        .font_size = FONT_16,
        .font_type = FONT_ST,
        .text_enc  = FONT_ENC_UTF8,
    });
}

static void sd_default_display_init(void)
{
    app_default_display_register(_sd_default_show);
}
sw_app_initcall(sd_default_display_init);