/**
 * @file    app_render.c
 * @brief   文字/图形渲染实现 — 数据驱动字库引擎
 *
 * 字库布局: (字号, 编码, 字型) 三元组在 Flash 中顺序拼接。
 * g_font_lib 描述每个三元组的总字节数，按 Flash 顺序排列。
 * 新增字号/字型只需追加条目。
 */

#include "app_render.h"

#include <string.h>
#include "text_cvt.h"
#include "initcall.h"
#include "dev_w25qxx.h"

/* ---- 字库单元描述 ---- */
typedef struct {
    font_key_t key;
    uint32_t unit_size; /* 该三元组的总字节数 */
} font_unit_t;

#define N_ASC_CHARS (96U)
#define N_GBK_CHARS (23940U)

/* 字库单元字节数: ASCII = (size/2)宽 × size高 × N_ASC_CHARS字; GBK = size宽 × size高 × N_GBK_CHARS */
#define ASC_UNIT(sz) ((uint32_t)(sz) * (((sz) / 2 + 7) / 8) * N_ASC_CHARS)
#define GBK_UNIT(sz) ((uint32_t)(sz) * (((sz) + 7) / 8) * N_GBK_CHARS)

/* 字库描述表 — 顺序必须与 Flash 中字库单元的排列一致 */
static const font_unit_t g_font_lib[] = {
    /* 14号 */
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(14)},
    {{.size = 14, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(14)},
    /* 16号 */
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(16)},
    {{.size = 16, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(16)},
    /* 20号 */
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(20)},
    {{.size = 20, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(20)},
    /* 24号 */
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(24)},
    {{.size = 24, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(24)},
    /* 32号 */
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_ST}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_FS}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_KT}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_ASCII, .type = FONT_HT}, ASC_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_ST}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_FS}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_KT}, GBK_UNIT(32)},
    {{.size = 32, .charset = FONT_ENC_GBK, .type = FONT_HT}, GBK_UNIT(32)},
};

/* ---- 内部: bytes_per_char ---- */
static inline uint16_t _glyph_bytes(font_key_t k)
{
    uint8_t w = (k.charset == FONT_ENC_ASCII) ? k.size / 2 : k.size;
    return (uint16_t)k.size * ((w + 7) / 8);
}

/* ---- 内部: 字符宽度 ---- */
static inline uint8_t _glyph_width_px(font_key_t k)
{
    return (k.charset == FONT_ENC_ASCII) ? k.size / 2 : k.size;
}

/* ---- 内部: 查找字库单元起始偏移 ---- */
static uint32_t _font_offset(const font_key_t *key)
{
    uint32_t off = 0;
    for (uint8_t i = 0; i < sizeof(g_font_lib) / sizeof(g_font_lib[0]); i++) {
        if (memcmp(&g_font_lib[i].key, key, sizeof(font_key_t)) == 0)
            return off;
        off += g_font_lib[i].unit_size;
    }
    return 0;
}

/* ---- 内部: 单个字符在 Flash 中的地址 ---- */
static uint32_t _char_addr(const font_key_t *key, const uint8_t *ch)
{
    uint32_t base  = _font_offset(key);
    uint16_t bytes = _glyph_bytes(*key);

    // ascii: 1字节, ch[0] = 字符码
    if (key->charset == FONT_ENC_ASCII)
        return base + (ch[0] - 0x20) * bytes;

    // GBK: 2字节, ch[0]=高字节, ch[1]=低字节
    uint32_t idx = (uint32_t)(ch[0] - 0x81) * 190 + (ch[1] >= 0x80 ? ch[1] - 0x41 : ch[1] - 0x40);
    return base + idx * bytes;
}

/* ---- 判断两字节是否为合法 GBK 码 ---- */
static bool _is_gbk(uint8_t high, uint8_t low)
{
    return (high >= 0x81 && high <= 0xFE) && (low >= 0x40 && low <= 0xFE && low != 0x7F);
}

/* ---- 注册的句柄 ---- */
static dev_display_t *s_render_display;
static dev_storage_t *s_render_font;

/* ---- 模块自注册，依赖storage和display模块）---- */
static void _render_init(void)
{
    s_render_display = dev_display_get();
    s_render_font    = dev_w25qxx_get();
}
sw_app_initcall(_render_init);

/* ---- 渲染分支（各功能静态内联）---- */

static inline void _render_text(const render_cfg_t *cfg)
{
    // 入口参数检查
    if (!cfg->text || !cfg->len || !cfg->font_size)
        return;
    if (!cfg->w || !cfg->h)
        return;

    font_key_t gbk_key = {.size = cfg->font_size, .type = cfg->font_type, .charset = FONT_ENC_GBK};
    font_key_t asc_key = {.size = gbk_key.size, .type = gbk_key.type, .charset = FONT_ENC_ASCII};

    uint16_t cur_x = cfg->x, cur_y = cfg->y;
    static uint8_t font_buf[512];
    static char text_buf[256];
    uint16_t text_len;

    if (cfg->text_enc == FONT_ENC_UTF8) {
        uint32_t out_len = sizeof(text_buf);
        UTF8ToGBK(cfg->text, cfg->len, text_buf, &out_len);
        text_len = (uint16_t)out_len;
    } else {
        uint16_t n = cfg->len < sizeof(text_buf) ? cfg->len : sizeof(text_buf);
        memcpy(text_buf, cfg->text, n);
        text_len = n;
    }

    /* ---- 测量趟：记录每行宽度（用于逐行对齐） ---- */
    uint16_t line_widths[32];
    uint8_t line_count = 0;
    uint16_t line_w    = 0;
    uint16_t line_h    = gbk_key.size;
    uint16_t char_pos  = 0;

    while (char_pos < text_len) {
        if (text_buf[char_pos] == '\n') {
            line_widths[line_count++] = line_w;
            line_w                    = 0;
            char_pos++;
            continue;
        }

        uint8_t glyph_w;
        if (text_buf[char_pos] >= 0x20 && text_buf[char_pos] <= 0x7F) {
            glyph_w = _glyph_width_px(asc_key);
            char_pos += 1;
        } else if (char_pos + 1 < text_len && _is_gbk((uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1])) {
            glyph_w = _glyph_width_px(gbk_key);
            char_pos += 2;
        } else {
            char_pos++;
            continue;
        }

        if (line_w + glyph_w > cfg->w) {
            if (cfg->style && cfg->style->word_wrap) {
                line_widths[line_count++] = line_w;
                line_w                    = glyph_w;
            }
            /* 不换行：超出部分截断，不计入宽度 */
        } else {
            line_w += glyph_w;
        }
    }
    line_widths[line_count++] = line_w; /* 最后一行 */

    /* ---- 垂直对齐 ---- */
    uint16_t text_h = line_count * line_h;
    if (cfg->style) {
        if (cfg->style->v_align == ALIGN_CENTER && cfg->h > text_h)
            cur_y += (cfg->h - text_h) / 2;
        else if (cfg->style->v_align == ALIGN_RIGHT_DOWN && cfg->h > text_h)
            cur_y += (cfg->h - text_h);
    }

    /* ---- 渲染趟：逐行独立水平对齐 ---- */
    uint8_t line_idx       = 0;
    uint16_t line_origin_x = cfg->x;
    if (cfg->style) {
        if (cfg->style->h_align == ALIGN_CENTER)
            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
            line_origin_x += (cfg->w - line_widths[line_idx]);
    }
    cur_x    = line_origin_x;
    char_pos = 0;

    while (char_pos < text_len) {
        if (text_buf[char_pos] == '\n') {
            cur_y += line_h;
            line_idx++;
            line_origin_x = cfg->x;
            if (cfg->style) {
                if (cfg->style->h_align == ALIGN_CENTER)
                    line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                    line_origin_x += (cfg->w - line_widths[line_idx]);
            }
            cur_x = line_origin_x;
            char_pos++;
            continue;
        }

        if (text_buf[char_pos] >= 0x20 && text_buf[char_pos] <= 0x7F) {
            uint8_t glyph_w = _glyph_width_px(asc_key);

            if (cur_x + glyph_w > cfg->w) {
                if (cfg->style && cfg->style->word_wrap) {
                    cur_y += line_h;
                    line_idx++;
                    line_origin_x = cfg->x;
                    if (cfg->style) {
                        if (cfg->style->h_align == ALIGN_CENTER)
                            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - line_widths[line_idx]);
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos++;
                    continue;
                }
            }

            uint8_t ch_byte = (uint8_t)text_buf[char_pos];
            uint32_t addr   = _char_addr(&asc_key, &ch_byte);
            dev_storage_read(s_render_font, addr, font_buf, _glyph_bytes(asc_key));
            dev_display_fill(s_render_display, cur_x, cur_y, glyph_w, asc_key.size, COLOR_BLACK);
            dev_display_draw_bitmap(s_render_display, cur_x, cur_y, glyph_w, asc_key.size, font_buf, cfg->color);

            cur_x += glyph_w;
            char_pos++;
            continue;

        } else if (char_pos + 1 < text_len && _is_gbk((uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1])) {
            uint8_t glyph_w = _glyph_width_px(gbk_key);

            if (cur_x + glyph_w > cfg->w) {
                if (cfg->style && cfg->style->word_wrap) {
                    cur_y += line_h;
                    line_idx++;
                    line_origin_x = cfg->x;
                    if (cfg->style) {
                        if (cfg->style->h_align == ALIGN_CENTER)
                            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                        else if (cfg->style->h_align == ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - line_widths[line_idx]);
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos += 2;
                    continue;
                }
            }

            uint8_t gbk_ch[2] = {(uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1]};
            uint32_t addr     = _char_addr(&gbk_key, gbk_ch);
            dev_storage_read(s_render_font, addr, font_buf, _glyph_bytes(gbk_key));
            dev_display_fill(s_render_display, cur_x, cur_y, glyph_w, gbk_key.size, COLOR_BLACK);
            dev_display_draw_bitmap(s_render_display, cur_x, cur_y, glyph_w, gbk_key.size, font_buf, cfg->color);

            cur_x += glyph_w;
            char_pos += 2;
            continue;

        } else {
            char_pos++;
        }
    }
}

static inline void _render_bitmap(const render_cfg_t *cfg)
{
    if (!cfg->w || !cfg->h || !cfg->bitmap) return;

    dev_display_draw_bitmap(s_render_display, cfg->x, cfg->y, cfg->w, cfg->h, cfg->bitmap, cfg->color);
}

static inline void _render_fill(const render_cfg_t *cfg)
{
    uint16_t w = cfg->w, h = cfg->h;
    if (!w || !h) {
        w = s_render_display->screen_rows;
        h = s_render_display->screen_cols;
    }
    dev_display_fill(s_render_display, cfg->x, cfg->y, w, h, cfg->color);
}

/* ---- 渲染跳表 ---- */
typedef void (*render_fn_t)(const render_cfg_t *);
static const render_fn_t g_render_fn[] = {
    [RENDER_TEXT]   = _render_text,
    [RENDER_BITMAP] = _render_bitmap,
    [RENDER_FILL]   = _render_fill,
};

/* ---- 公开 API：tagged union 分派 ---- */
void app_render(const render_cfg_t *cfg)
{
    if (!cfg || !s_render_display) return;
    if (cfg->type < sizeof(g_render_fn) / sizeof(g_render_fn[0]) && g_render_fn[cfg->type])
        g_render_fn[cfg->type](cfg);
}
