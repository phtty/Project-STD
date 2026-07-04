/**
 * @file    app_render.h
 * @brief   文字/图形渲染 — tagged union 统一入参
 *
 * 字库存储为 (字号, 编码, 字型) 三元组的顺序拼接。
 * 新增字号/字型只需在 g_font_lib 追加条目。
 * 新增渲染类型只需在 render_type_t 和 union 中追加。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dev_display.h"
#include "dev_storage.h"

/* ---- 字号（像素高度，ASCII 半宽 = size/2）---- */
typedef enum {
    FONT_14 = 14,
    FONT_16 = 16,
    FONT_20 = 20,
    FONT_24 = 24,
    FONT_32 = 32,
} font_size_t;

/* ---- 字型 ---- */
typedef enum {
    FONT_ST = 0, /* 宋体 */
    FONT_FS = 1, /* 仿宋 */
    FONT_KT = 2, /* 楷体 */
    FONT_HT = 3, /* 黑体 */
} font_type_t;

/* ---- 编码类型 ---- */
typedef enum {
    FONT_ENC_ASCII = 0, /* 字库编码 — ASCII 单字节 */
    FONT_ENC_GBK   = 1, /* 字库编码 — GBK  双字节 */
    FONT_ENC_UTF8  = 2, /* 输入文本编码 — 内部自动转 GBK */
} font_enc_t;

/* ---- 字库三元组：内部检索 key，调用方无需接触 ---- */
typedef struct {
    font_size_t size;   /* 字号 */
    font_enc_t charset; /* 字库编码 (ASCII/GBK) — 渲染器内部按字符自动填充 */
    font_type_t type;   /* 字型 */
} font_key_t;

/* ---- 水平/垂直对齐 ---- */
typedef enum {
    ALIGN_LEFT_UP    = 0, /* 左对齐 / 上对齐 */
    ALIGN_CENTER     = 1, /* 居中 */
    ALIGN_RIGHT_DOWN = 2, /* 右对齐 / 下对齐 */
} align_t;

/* ---- 渲染风格（文字专属）---- */
typedef struct {
    align_t h_align; /* 水平对齐 */
    align_t v_align; /* 垂直对齐 */
    bool word_wrap;  /* 超宽时自动换行 */
} render_style_t;

/* ---- 渲染类型：告诉 app_render 如何解析 union ---- */
typedef enum {
    RENDER_TEXT   = 0, /* 文字渲染 — 使用 text/len/font_key/style/text_enc */
    RENDER_BITMAP = 1, /* 位图渲染 — 使用 bitmap/w/h           */
    RENDER_FILL   = 2, /* 矩形填充 — 使用公共字段 x/y/w/h/color (w/h=0 全屏) */
} render_type_t;

/* ---- 统一渲染参数 — tagged union — type 决定哪个 union 分支生效 ---- */
typedef struct {
    /* 公共 — 调用方设置后渲染器只读 */
    const uint16_t x, y;         /* 目标起点 */
    const uint16_t w, h;         /* 目标宽高 (fill 时 w/h=0 表示全屏) */
    const display_color_t color; /* 绘制颜色 */
    const render_type_t type;    /* 标签: 指定使用哪个 union 分支 */

    union {
        /* RENDER_TEXT — 文字专属 */
        struct {
            const char *text;            /* 字符串 */
            const uint16_t len;          /* 字符串长度（字节数） */
            const font_size_t font_size; /* 字号 */
            const font_type_t font_type; /* 字型 */
            const render_style_t *style; /* 对齐/换行 (NULL=默认) */
            const font_enc_t text_enc;   /* 输入文本编码 (UTF8需转换/GBK直通) */
        };

        /* RENDER_BITMAP — 位图专属 */
        const uint8_t *const bitmap; /* 位图数据, 每行 (w+7)/8 字节, MSB first */

        /* RENDER_FILL — 无专属字段, 只用公共的 x/y/w/h/color */
    };
} render_cfg_t;

/* ---- API（模块自注册 sw_app_initcall，调用方无需传 display/font 句柄）---- */

/** @brief 统一渲染入口 — 根据 cfg->type 分派到内部实现 */
void app_render(const render_cfg_t *cfg);
