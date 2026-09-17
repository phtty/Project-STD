/**
 * @file    app_render.c
 * @brief   文字/图形渲染实现 — 数据驱动字库引擎
 *
 * 字库布局: (字号, 编码, 字型) 三元组在 Flash 中顺序拼接。
 * g_font_lib 描述每个三元组的总字节数，按 Flash 顺序排列。
 * 新增字号/字型只需追加条目。
 */

#include "app_render.h"

#include <stdio.h>
#include <string.h>
#include "cmsis_os2.h"
#include "text_cvt.h"
#include "initcall.h"
#include "crc_utils.h"
#include "dev_w25qxx.h"
#include "cfg_record.h"   /* CFG_REC_OK */
#include "app_cfg_sched.h" /* 显存持久化走调度器 */

/* ---- 字库单元描述 ---- */
typedef struct {
    font_key_t key;
    uint32_t unit_size; /* 该三元组的总字节数 */
} font_unit_t;

/* 字库规模宏（N_ASC_CHARS / N_GBK_CHARS / ASC_UNIT / GBK_UNIT / FONT_LIB_TOTAL_BYTES）
 * 定义于 app_render.h —— 持久化区要靠 FONT_LIB_TOTAL_BYTES 做容量契约。 */

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

/* 自适应字号的候选集合（从大到小尝试，索引 0 为 SELF_ADAPT 占位不参与选择） */
static const font_size_t font_size_table[] = {
    FONT_SELF_ADAPT,
    FONT_14,
    FONT_16,
    FONT_20,
    FONT_24,
    FONT_32,
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

/* 显存持久化的调度器句柄（地址/归属/CRC/去重都由调度器管） */
static uint8_t s_persist_id = 0xFF;

/* 保护载荷组装缓冲（s_persist_buf，定义见文件末尾的持久化段）。
   放在这里是因为 _render_init 要创建它，而那在文件前部。 */
static osMutexId_t s_persist_lock;

static const cfg_sched_desc_t s_render_persist_desc = {
    .name    = "render_persist",
    .version = RENDER_PERSIST_VERSION,
    /* 不在启动加载遍里恢复显示：上电该显示什么由 app_boot 的 app_default_display
       决定（先试 restore，失败再画默认内容）。调度器只负责存取。 */
    .load = NULL,
};

static void _render_persist_register(void)
{
    s_persist_id = app_cfg_sched_register(&s_render_persist_desc);
    if (s_persist_id == 0xFF)
        printf("[render] 显存持久化注册失败，本次上电不落盘\n");
}
/* sw_dev(2)：注册只需早于 sw_app(3) 的启动加载遍，以及早于任何 save。 */
sw_dev_initcall(_render_persist_register);

/* ---- 模块自注册，依赖storage和display模块）---- */
static void _render_init(void)
{
    s_render_display = dev_display_get();
    s_render_font    = dev_w25qxx_get();

    /* 运行期交叉校验：描述表实际求和必须等于编译期常量。
     * app_render.h 的 _Static_assert 只能验公式，挡不住"g_font_lib[] 被手改一行"
     * （例如某条硬编码了字节数、或加了字号却漏改 FONT_LIB_TOTAL_BYTES）——
     * 那会让其后每个单元的 Flash 偏移整体错位，字库取到乱码。 */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeof(g_font_lib) / sizeof(g_font_lib[0]); i++)
        sum += g_font_lib[i].unit_size;
    if (sum != FONT_LIB_TOTAL_BYTES)
        printf("[render] 字库描述表求和 %u != 编译期常量 %u，字库偏移已错位\n", (unsigned)sum,
               (unsigned)FONT_LIB_TOTAL_BYTES);

    /* 持久化区的地址/容量门槛不再由本模块计算 —— 配置调度器统一管
     * （见 app_cfg_sched.c 的 s_ready：要求"字库之后还放得下整个配置区"）。 */

    /* 载荷组装锁。在本层创建：RTOS 已启动，且早于任何协议任务可能触发的 save。 */
    const osMutexAttr_t persist_attr = {.name = "render_persist", .attr_bits = osMutexPrioInherit};
    s_persist_lock                   = osMutexNew(&persist_attr);
}
sw_app_initcall(_render_init);

/* ---- 渲染分支（各功能静态内联）---- */

static inline void _render_text(const render_cfg_t *cfg)
{
    // 入口参数检查
    if (!cfg->text || !cfg->len)
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

    /* 字号自适应：按文本长度与渲染区域容量，从最大字号开始选择能容纳的最大字号，默认最小字号 14 */
    if (cfg->font_size == FONT_SELF_ADAPT) {
        gbk_key.size = FONT_14;
        asc_key.size = FONT_14;
        for (int8_t i = (int8_t)(sizeof(font_size_table) / sizeof(font_size_table[0])) - 1; i >= 1; i--) {
            uint16_t h_res = cfg->h / font_size_table[i];
            uint16_t w_res = cfg->w / (font_size_table[i] / 2);
            if (text_len <= h_res * w_res) {
                gbk_key.size = font_size_table[i];
                asc_key.size = font_size_table[i];
                break;
            }
        }
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

/* ================================================================
 *  持久化显示 — 将显存以位图格式保存/恢复到存储设备
 *
 *  pixel_map (逐像素颜色) → bitmap (1bit/pixel + 单色)，大幅压缩闪存占用。
 *  恢复阶段 fill(BLACK) + draw_bitmap(color) 重建 pixel_map。
 *
 *  载荷的归属/版本/长度/CRC 由配置调度器统一管（记录名 "render_persist"），
 *  本模块不再自带头内魔数与 CRC，也不再自己算 Flash 地址。
 * ================================================================ */

/** 位图区按"全工程最大模组"定长，故载荷缓冲也按它定长（见 app_render.h） */
static uint8_t s_persist_buf[RENDER_PERSIST_PAYLOAD_MAX];

/* s_persist_buf 由 s_persist_lock 保护（声明见文件前部的句柄段）：
   save 会来自**不同任务**（RLS 任务经 app_rls_cmd、LDI 任务经 app_vms_ctrl），
   而载荷是"先组装进这个缓冲、再交给调度器落盘"。调度器自己的锁只覆盖落盘那一步，
   覆盖不到组装阶段 —— 两个任务同时组装的话，先到者的缓冲会在落盘前被后到者改写，
   存下去的是两张屏的混合体。嵌套顺序固定为"先本锁、后调度器锁"，无反向获取故不会死锁。 */

/** @brief 组装/解析载荷期间持锁；锁不可用时退化为不锁（启动早期尚无竞争） */
static void _persist_lock(void)
{
    if (s_persist_lock) osMutexAcquire(s_persist_lock, osWaitForever);
}

static void _persist_unlock(void)
{
    if (s_persist_lock) osMutexRelease(s_persist_lock);
}

/** @brief 本屏的位图字节数；同时做上界检查。越界返回 0。 */
static uint16_t _persist_bm_bytes(const dev_display_t *d)
{
    uint32_t row_bytes = ((uint32_t)d->screen_rows + 7U) / 8U;
    uint32_t bm_bytes  = (uint32_t)d->screen_cols * row_bytes;

    /* 本工程真 dev_display.h 没有编译期几何宏，位图区只能按"全工程最大模组"定长。
     * 换上更大的模组时在这里拦下 —— 拒绝保存而不是越界写。 */
    if (bm_bytes > RENDER_PERSIST_BITMAP_MAX) {
        printf("[render] 本屏位图 %u 字节超过持久化上限 %u，已跳过\n", (unsigned)bm_bytes,
               (unsigned)RENDER_PERSIST_BITMAP_MAX);
        return 0;
    }
    return (uint16_t)bm_bytes;
}

void app_render_save(void)
{
    dev_display_t *d = s_render_display;
    if (!d || s_persist_id == 0xFF) return;

    uint16_t rows     = d->screen_rows;
    uint16_t cols     = d->screen_cols;
    uint16_t bm_bytes = _persist_bm_bytes(d);
    if (!bm_bytes) return;
    uint16_t row_bytes = (rows + 7) / 8;

    _persist_lock(); /* 直到落盘返回前都持有：组装缓冲不能被另一个任务改写 */

    render_persist_t *r = (render_persist_t *)s_persist_buf;
    memset(r->bitmap, 0, bm_bytes);

    uint8_t color = COLOR_BLACK;
    for (uint16_t y = 0; y < cols; y++) {
        for (uint16_t x = 0; x < rows; x++) {
            uint8_t px = d->pixel_map[y * rows + x];
            if (px != COLOR_BLACK) {
                r->bitmap[y * row_bytes + x / 8] |= (uint8_t)(0x80 >> (x % 8));
                if (color == COLOR_BLACK) color = px;
            }
        }
    }

    r->screen_rows = rows;
    r->screen_cols = cols;
    r->color       = color;

    /* 落盘失败必须可见：此前调用方一律忽略返回值，现场只表现为
     * "改了显示、重启回到旧内容"，无从查起。 */
    uint32_t len = (uint32_t)sizeof(render_persist_t) + bm_bytes;
    if (app_cfg_sched_save(s_persist_id, s_persist_buf, (uint16_t)len) != 0)
        printf("[render] 显存持久化失败（%u 字节）\n", (unsigned)len);

    _persist_unlock();
}

bool app_render_restore(void)
{
    dev_display_t *d = s_render_display;
    if (!d || s_persist_id == 0xFF)
        return false;

    uint16_t rows     = d->screen_rows;
    uint16_t cols     = d->screen_cols;
    uint16_t bm_bytes = _persist_bm_bytes(d);
    if (!bm_bytes) return false;

    /* 传入的容量取"本屏实际需要"：记录里的 len 与之不符会被 cfg_record_load
     * 判为 INVALID —— 这正是我们要的（换了模组则旧显存不适用）。 */
    uint16_t payload_cap = (uint16_t)(sizeof(render_persist_t) + bm_bytes);
    uint16_t rec_len     = 0;

    /* 与 save 共用同一个组装缓冲，故同样要持锁 */
    _persist_lock();

    if (app_cfg_sched_load(s_persist_id, s_persist_buf, payload_cap, &rec_len) != CFG_REC_OK)
        goto fail;
    if (rec_len != payload_cap) goto fail;

    render_persist_t *r = (render_persist_t *)s_persist_buf;
    /* 几何已由 len 比对隐含校验（len 由本屏几何算出），这里再核一次字段，
     * 防止"长度碰巧相同但内容不是本屏"的情况。 */
    if (r->screen_rows != rows || r->screen_cols != cols) goto fail;

    dev_display_fill(d, 0, 0, rows, cols, COLOR_BLACK);
    dev_display_draw_bitmap(d, 0, 0, rows, cols, r->bitmap, (display_color_t)r->color);
    _persist_unlock();
    return true;

fail:
    _persist_unlock();
    return false;
}
