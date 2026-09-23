/**
 * @file    app_render.c
 * @brief   文字/图形渲染实现 — 数据驱动字库引擎
 *
 * 字库布局: (字号, 编码, 字型) 三元组在 Flash 中顺序拼接。
 * 具体有哪些三元组、各自多大、怎么索引，**都是板级事实**（两板字库不是同一版：
 * 3833024 是 GBK/14-16-20-24-32，5006048 是 GB2312/16-24-32-48），由
 * boards/<板>/Src/app_font_lib_board.c 的 g_board_font_lib 提供，本模块只消费。
 */

#include "app_render.h"

#include <stdio.h>
#include <string.h>
#include "board.h" /* BOARD_FONT_LIB_TOTAL_BYTES（编译期容量契约） */
#include "cmsis_os2.h"
#include "text_cvt.h"
#include "initcall.h"
#include "crc_utils.h"
#include "dev_w25qxx.h"
#include "dev_cfg_record.h"   /* DEV_CFG_RECORD_STATE_OK */
#include "app_cfg_sched.h" /* 显存持久化走调度器 */

/* 字库表、可用字号集合、总字节数全部是板级事实（两板的字号集合与字符集都不同：
 * 3833024 是 GBK 14/16/20/24/32，5006048 是 GB2312 16/24/32/48），见
 * app_render.h 的 app_font_lib_desc_t 与 board.h 的 BOARD_FONT_LIB_TOTAL_BYTES。
 * 本文件不再持有任何具体数字。 */

/* ---- 内部: bytes_per_char ---- */
static inline uint16_t _glyph_bytes(app_font_key_t k)
{
    uint8_t w = (k.charset == APP_FONT_ENC_ASCII) ? k.size / 2 : k.size;
    return (uint16_t)k.size * ((w + 7) / 8);
}

/* ---- 内部: 字符宽度 ---- */
static inline uint8_t _glyph_width_px(app_font_key_t k)
{
    return (k.charset == APP_FONT_ENC_ASCII) ? k.size / 2 : k.size;
}

/* ---- 内部: 把请求字号解析为本板实际可用的字号 ----
 *
 * 两板的字号集合不同（3833024 无 48，5006048 无 14/20），而请求字号来自运行期
 * 协议（LDI 的 0BH 等），编译期拦不住。**回落到最近邻。**
 *
 * 旧实现是让 _font_offset 找不到就返回偏移 0 —— 那会拿本板第 0 个字库单元当该
 * 字号用，渲染出完全无关的内容，且不报错、不打日志，是最难查的一种失效。
 *
 * 并列时取**较小**的一个：调用方的显示区域是按请求字号预留的，取小的不会溢出。
 * sizes[] 必须升序（app_font_lib_desc_t 的约定），下面"严格小于才替换"正是靠它
 * 保证并列时留下先遇到的、也就是更小的那个。 */
static app_font_size_t _resolve_size(app_font_size_t want)
{
    const app_font_lib_desc_t *d = &g_board_font_lib;
    if (d->size_count == 0) return (app_font_size_t)0; /* 板级表为空：调用方据此放弃渲染 */

    app_font_size_t best   = d->sizes[0];
    uint32_t    best_d = (want > best) ? (uint32_t)(want - best) : (uint32_t)(best - want);

    for (uint8_t i = 1; i < d->size_count; i++) {
        app_font_size_t s   = d->sizes[i];
        uint32_t    dif = (want > s) ? (uint32_t)(want - s) : (uint32_t)(s - want);
        if (dif < best_d) {
            best_d = dif;
            best   = s;
        }
    }
    return best;
}

/* ---- 内部: 查字库单元并给出其 Flash 起始偏移 ----
 * 单元线性连续排列，偏移 = 前 i 项 unit_size 之和（见 app_font_lib_desc_t）。 */
static bool _find_unit(const app_font_key_t *key, uint32_t *offset)
{
    uint32_t off = 0;
    for (uint16_t i = 0; i < g_board_font_lib.lib_count; i++) {
        const app_font_unit_t *u = &g_board_font_lib.lib[i];
        /* 逐字段比较而非 memcmp：结构体可能有填充字节，memcmp 会连填充一起比 */
        if (u->key.size == key->size && u->key.charset == key->charset && u->key.type == key->type) {
            *offset = off;
            return true;
        }
        off += u->unit_size;
    }
    return false;
}

/* ---- 内部: 单个字符在 Flash 中的地址 ----
 *
 * 单元缺失（板级表缺项）返回 false，**调用方跳过该字** —— 不要退回偏移 0 去读，
 * 那是别的字型的字形。索引式按 g_board_font_lib 指定的方案选：两版字库的 ASCII 起点
 * （0x20 / 0x00）与汉字区位基准（GBK 190 进制 / GB2312 94 进制）都不同。 */
static bool _char_addr(const app_font_key_t *key, const uint8_t *ch, uint32_t *addr)
{
    uint32_t base;
    if (!_find_unit(key, &base)) return false;

    uint16_t bytes = _glyph_bytes(*key);

    if (key->charset == APP_FONT_ENC_ASCII) {
        /* ASCII: 1字节, ch[0] = 字符码。起点由字库决定，不是想当然的 0x20 ——
           5006048 的映像就是 0x00 起、按原始码索引的 128 槽表。 */
        if (ch[0] < g_board_font_lib.asc_index_base) return false;
        *addr = base + ((uint32_t)ch[0] - g_board_font_lib.asc_index_base) * bytes;
        return true;
    }

    /* 汉字: 2字节, ch[0]=高字节, ch[1]=低字节 */
    uint32_t idx;
    if (g_board_font_lib.gb_index == APP_FONT_IDX_KIND_GB2312) {
        /* hi 从 0x81 起也要拦：_is_gbk 放行 0x81~0xFE，而 (hi-0xA1) 在 hi<0xA1 时
           下溢；lo=0xA0 同理（参考工程对 0xA1~0xA9 区正是放行 lo>=0xA0 的）。
           两处下溢都会回绕成一个巨大索引，读到**别的字型的尾部** —— 真实可达。 */
        if (ch[0] < 0xA1 || ch[1] < 0xA1) return false;
        idx = 94U * (uint32_t)(ch[0] - 0xA1) + (uint32_t)(ch[1] - 0xA1);
    } else {
        if (ch[0] < 0x81 || ch[1] < 0x40) return false;
        idx = (uint32_t)(ch[0] - 0x81) * 190U + (ch[1] >= 0x80 ? ch[1] - 0x41 : ch[1] - 0x40);
    }
    *addr = base + idx * bytes;
    return true;
}

/* ---- 判断两字节是否为合法 GBK 码 ---- */
static bool _is_gbk(uint8_t high, uint8_t low)
{
    return (high >= 0x81 && high <= 0xFE) && (low >= 0x40 && low <= 0xFE && low != 0x7F);
}

/* ---- 注册的句柄 ---- */
static dev_display_t *s_render_display;
static dev_storage_t *s_render_font;

/* ---- 渲染目标（见 app_render.h）----
 * 未设置时回落"直写实屏"，与这条缝引入之前的行为完全一致。 */
static const app_render_target_t *s_target;
static const app_render_persist_hook_fn_t *s_persist_hook;

/* ---- "这一帧要落盘"的请求位 ----
 *
 * **只在有画布时才用**（多卡主卡）：渲染返回时内容还在画布上、没落屏，此刻读实屏
 * 存下去的是**上一帧** —— 这正是 `app_rls_cmd.c` / `app_vms_ctrl.c` 那三个
 * "render 完立刻 save" 调用点的旧缺陷。所以这里只记请求，由 `app_screen` 在
 * **落屏之后**（`commit_bitmap` / `commit_self`）取走。
 *
 * 没有画布时（单卡、或从卡直写实屏）画的就是实屏，当场存即可 —— 见 app_render()。 */
static volatile bool s_persist_req;

/** @brief 取走"要落盘"的请求（清标志）；由落屏路径调用 */
bool app_render_take_persist_req(void)
{
    const bool r = s_persist_req;
    s_persist_req = false;
    return r;
}

/** @brief **不取走**、只看：级联开轮时用它决定这一轮的 IMAGE 带不带 persist
 *         （帧在落屏**之前**发出去，那时还不能取走标志） */
bool app_render_peek_persist_req(void)
{
    return s_persist_req;
}

/* ---- "正在渲染"的计数（见 app_render_busy 的说明）----
 *
 * 用计数而不是 bool：渲染内部还有可能回调（持久化钩子），将来若有人嵌套调用，
 * 计数不会让闸提前打开。 */
static volatile int s_render_busy;

bool app_render_busy(void)
{
    return s_render_busy > 0;
}

static void _direct_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t c)
{
    dev_display_fill((dev_display_t *)ctx, x, y, w, h, c);
}
static void _direct_bitmap(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *bm, dev_display_color_t c)
{
    dev_display_draw_bitmap((dev_display_t *)ctx, x, y, w, h, bm, c);
}
static void _direct_set_pixel(void *ctx, uint16_t x, uint16_t y, dev_display_color_t c)
{
    dev_display_set_pixel((dev_display_t *)ctx, x, y, c);
}

/** @brief 取当前渲染目标；未显式设置时按实屏现搭一个。
 *
 *  每次现搭而不是缓存：几何来自 dev_display_t，而它在 hw initcall 里才注册，
 *  缓存在模块静态里会锁住一个可能为 NULL 的早期值。现搭是 6 次赋值，可以忽略。 */
static const app_render_target_t *_rt(void)
{
    static app_render_target_t direct;
    if (s_target) return s_target;

    direct.fill      = _direct_fill;
    direct.bitmap    = _direct_bitmap;
    direct.set_pixel = _direct_set_pixel;
    direct.ctx       = s_render_display;
    direct.rows      = s_render_display ? s_render_display->screen_rows : 0;
    direct.cols      = s_render_display ? s_render_display->screen_cols : 0;
    return &direct;
}

void app_render_set_target(const app_render_target_t *t)
{
    s_target = t;
}

void app_render_set_persist_hook(const app_render_persist_hook_fn_t *h)
{
    s_persist_hook = h;
}

/* 显存持久化的调度器句柄（地址/归属/CRC/去重都由调度器管） */
static uint8_t s_persist_id = 0xFF;

/* 保护载荷组装缓冲（s_persist_buf，定义见文件末尾的持久化段）。
   放在这里是因为 _render_init 要创建它，而那在文件前部。 */
static osMutexId_t s_persist_lock;

static const app_cfg_sched_desc_t s_render_persist_desc = {
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

    /* 运行期交叉校验。三条都是"错了不会报错、只会让字库取到乱码"的失效模式，
       所以宁可每次上电多说一句。 */
    uint32_t sum = 0;
    for (uint16_t i = 0; i < g_board_font_lib.lib_count; i++)
        sum += g_board_font_lib.lib[i].unit_size;

    if (sum != g_board_font_lib.total_bytes)
        printf("[render] 板级字库表求和 %u != 表内 total_bytes %u，其后每个单元的偏移都错位了\n",
               (unsigned)sum, (unsigned)g_board_font_lib.total_bytes);

    /* 表内 total_bytes 与编译期常量分居两处（前者板级 .c、后者 board.h）。不同步的
       后果不只是取字乱码 —— 配置区地址 = capacity - (blk+1)*4096，门槛按哪个值算的
       就是按哪个；常量偏小会让配置区落进字库区，首次擦写直接毁字库。 */
    if (g_board_font_lib.total_bytes != BOARD_FONT_LIB_TOTAL_BYTES)
        printf("[render] 板级字库 total_bytes %u != board.h 的 %u，配置区地址会算错\n",
               (unsigned)g_board_font_lib.total_bytes, (unsigned)BOARD_FONT_LIB_TOTAL_BYTES);

    /* sizes[] 必须升序 —— _resolve_size 的"并列取小"依赖这个前提，
       乱序时最近邻会挑错，且同样没有任何报错。 */
    for (uint8_t i = 1; i < g_board_font_lib.size_count; i++) {
        if (g_board_font_lib.sizes[i] <= g_board_font_lib.sizes[i - 1]) {
            printf("[render] 板级字号集合非升序（%u 号在 %u 号之后），最近邻回落会选错\n",
                   (unsigned)g_board_font_lib.sizes[i], (unsigned)g_board_font_lib.sizes[i - 1]);
            break;
        }
    }


    /* 持久化区的地址/容量门槛不再由本模块计算 —— 配置调度器统一管
     * （见 app_cfg_sched.c 的 s_storage_ready：要求"字库之后还放得下整个配置区"）。 */

    /* 载荷组装锁。在本层创建：RTOS 已启动，且早于任何协议任务可能触发的 save。 */
    const osMutexAttr_t persist_attr = {.name = "render_persist", .attr_bits = osMutexPrioInherit};
    s_persist_lock                   = osMutexNew(&persist_attr);
}
sw_app_initcall(_render_init);

/* ---- 渲染分支（各功能静态内联）---- */

static inline void _render_text(const app_render_cfg_t *cfg)
{
    // 入口参数检查
    if (!cfg->text || !cfg->len)
        return;
    if (!cfg->w || !cfg->h)
        return;

    app_font_key_t gbk_key = {.size = cfg->font_size, .type = cfg->font_type, .charset = APP_FONT_ENC_GBK};
    app_font_key_t asc_key = {.size = gbk_key.size, .type = gbk_key.type, .charset = APP_FONT_ENC_ASCII};

    uint16_t cur_x = cfg->x, cur_y = cfg->y;
    static uint8_t font_buf[512];
    static char text_buf[256];
    uint16_t text_len;

    if (cfg->text_enc == APP_FONT_ENC_UTF8) {
        uint32_t out_len = sizeof(text_buf);
        cvt_utf8_to_gbk(cfg->text, cfg->len, text_buf, &out_len);
        text_len = (uint16_t)out_len;
    } else {
        uint16_t n = cfg->len < sizeof(text_buf) ? cfg->len : sizeof(text_buf);
        memcpy(text_buf, cfg->text, n);
        text_len = n;
    }

    /* ---- 字号落定 ---- */
    const app_font_lib_desc_t *flib = &g_board_font_lib;
    if (flib->size_count == 0) return; /* 板级字库表为空：本板根本没有字库 */

    if (cfg->font_size == APP_FONT_SIZE_SELF_ADAPT) {
        /* 自适应：按文本长度与渲染区域容量，从最大字号开始选能容纳的最大字号。
           都不放得下时用最小字号（下面的初值），由换行/截断逻辑收尾。 */
        gbk_key.size = flib->sizes[0];
        asc_key.size = flib->sizes[0];
        for (int8_t i = (int8_t)flib->size_count - 1; i >= 0; i--) {
            uint16_t h_res = cfg->h / flib->sizes[i];
            uint16_t w_res = cfg->w / (flib->sizes[i] / 2);
            if (text_len <= h_res * w_res) {
                gbk_key.size = flib->sizes[i];
                asc_key.size = flib->sizes[i];
                break;
            }
        }
    } else {
        /* 请求字号不在本板上时回落到最近邻 —— 否则 _find_unit 找不到，会一路
           退化成"什么都不画"或（旧实现）拿偏移 0 的单元去读 */
        app_font_size_t r = _resolve_size(cfg->font_size);
        gbk_key.size  = r;
        asc_key.size  = r;
    }

    /* 三元组齐全性：缺项就整条不画。逐字失败会把同一条日志每字刷一遍，
       而缺项是板级表的问题，一次说清就够。 */
    uint32_t probe;
    if (!_find_unit(&gbk_key, &probe) || !_find_unit(&asc_key, &probe)) {
        printf("[render] 板级字库缺 %u 号/%u 字型，本次不渲染\n", (unsigned)gbk_key.size,
               (unsigned)gbk_key.type);
        return;
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
        if (cfg->style->v_align == APP_RENDER_ALIGN_CENTER && cfg->h > text_h)
            cur_y += (cfg->h - text_h) / 2;
        else if (cfg->style->v_align == APP_RENDER_ALIGN_RIGHT_DOWN && cfg->h > text_h)
            cur_y += (cfg->h - text_h);
    }

    /* ---- 渲染趟：逐行独立水平对齐 ---- */
    uint8_t line_idx       = 0;
    uint16_t line_origin_x = cfg->x;
    if (cfg->style) {
        if (cfg->style->h_align == APP_RENDER_ALIGN_CENTER)
            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
        else if (cfg->style->h_align == APP_RENDER_ALIGN_RIGHT_DOWN)
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
                if (cfg->style->h_align == APP_RENDER_ALIGN_CENTER)
                    line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                else if (cfg->style->h_align == APP_RENDER_ALIGN_RIGHT_DOWN)
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
                        if (cfg->style->h_align == APP_RENDER_ALIGN_CENTER)
                            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                        else if (cfg->style->h_align == APP_RENDER_ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - line_widths[line_idx]);
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos++;
                    continue;
                }
            }

            uint8_t  ch_byte = (uint8_t)text_buf[char_pos];
            uint32_t addr;
            /* 取址失败（板级表缺项或码位越界）就只跳过这个字，仍照常推进光标 ——
               否则后面的字会挤到同一个位置叠着画 */
            if (_char_addr(&asc_key, &ch_byte, &addr)) {
                dev_storage_read(s_render_font, addr, font_buf, _glyph_bytes(asc_key));
                const app_render_target_t *rt = _rt();
                rt->fill(rt->ctx, cur_x, cur_y, glyph_w, asc_key.size, DEV_DISPLAY_COLOR_BLACK);
                rt->bitmap(rt->ctx, cur_x, cur_y, glyph_w, asc_key.size, font_buf, cfg->color);
            }

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
                        if (cfg->style->h_align == APP_RENDER_ALIGN_CENTER)
                            line_origin_x += (cfg->w - line_widths[line_idx]) / 2;
                        else if (cfg->style->h_align == APP_RENDER_ALIGN_RIGHT_DOWN)
                            line_origin_x += (cfg->w - line_widths[line_idx]);
                    }
                    cur_x = line_origin_x;
                    if (cur_y + line_h > cfg->h) return;
                } else {
                    char_pos += 2;
                    continue;
                }
            }

            uint8_t  gbk_ch[2] = {(uint8_t)text_buf[char_pos], (uint8_t)text_buf[char_pos + 1]};
            uint32_t addr;
            if (_char_addr(&gbk_key, gbk_ch, &addr)) {
                dev_storage_read(s_render_font, addr, font_buf, _glyph_bytes(gbk_key));
                const app_render_target_t *rt = _rt();
                rt->fill(rt->ctx, cur_x, cur_y, glyph_w, gbk_key.size, DEV_DISPLAY_COLOR_BLACK);
                rt->bitmap(rt->ctx, cur_x, cur_y, glyph_w, gbk_key.size, font_buf, cfg->color);
            }

            cur_x += glyph_w;
            char_pos += 2;
            continue;

        } else {
            char_pos++;
        }
    }
}

static inline void _render_bitmap(const app_render_cfg_t *cfg)
{
    if (!cfg->w || !cfg->h || !cfg->bitmap) return;

    const app_render_target_t *rt = _rt();
    rt->bitmap(rt->ctx, cfg->x, cfg->y, cfg->w, cfg->h, cfg->bitmap, cfg->color);
}

static inline void _render_fill(const app_render_cfg_t *cfg)
{
    const app_render_target_t *rt = _rt();
    uint16_t w = cfg->w, h = cfg->h;
    if (!w || !h) {
        w = rt->rows;
        h = rt->cols;
    }
    rt->fill(rt->ctx, cfg->x, cfg->y, w, h, cfg->color);
}

/* ---- 渲染跳表 ---- */
typedef void (*app_render_fn_t)(const app_render_cfg_t *);
static const app_render_fn_t s_render_fn_table[] = {
    [APP_RENDER_TYPE_TEXT]   = _render_text,
    [APP_RENDER_TYPE_BITMAP] = _render_bitmap,
    [APP_RENDER_TYPE_FILL]   = _render_fill,
};

/* ---- 公开 API：tagged union 分派 ---- */
void app_render(const app_render_cfg_t *cfg)
{
    if (!cfg || !s_render_display) return;
    s_render_busy++;
    if (cfg->type < sizeof(s_render_fn_table) / sizeof(s_render_fn_table[0]) && s_render_fn_table[cfg->type])
        s_render_fn_table[cfg->type](cfg);
    s_render_busy--;

    /* ---- 持久化请求（见 app_render.h 的 persist 说明）----
     * **不能在这里直接存**：有画布时内容还在画布上、没落屏，此刻存下去是上一帧。 */
    if (!cfg->persist) return;

    if (s_target) {
        s_persist_req = true; /* 交给 app_screen，落屏之后取走 */
    } else {
        /* 没画布：画的就是实屏（单卡、或从卡直写实屏），此刻已在屏上 → 当场存 */
        app_render_save();
    }
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
    /* 装了逻辑画布就整体委托：那时"该存什么"由画布所有者决定 ——
       直接读 dev_display 会只存到主卡自己那块，与发给从卡的内容不是一回事。 */
    if (s_persist_hook && s_persist_hook->save) {
        s_persist_hook->save();
        return;
    }

    dev_display_t *d = s_render_display;
    if (!d || s_persist_id == 0xFF) return;

    uint16_t rows     = d->screen_rows;
    uint16_t cols     = d->screen_cols;
    uint16_t bm_bytes = _persist_bm_bytes(d);
    if (!bm_bytes) return;
    uint16_t row_bytes = (rows + 7) / 8;

    _persist_lock(); /* 直到落盘返回前都持有：组装缓冲不能被另一个任务改写 */

    app_render_persist_t *r = (app_render_persist_t *)s_persist_buf;
    memset(r->bitmap, 0, bm_bytes);

    uint8_t color = DEV_DISPLAY_COLOR_BLACK;
    for (uint16_t y = 0; y < cols; y++) {
        for (uint16_t x = 0; x < rows; x++) {
            uint8_t px = d->pixel_map[y * rows + x];
            if (px != DEV_DISPLAY_COLOR_BLACK) {
                r->bitmap[y * row_bytes + x / 8] |= (uint8_t)(0x80 >> (x % 8));
                if (color == DEV_DISPLAY_COLOR_BLACK) color = px;
            }
        }
    }

    r->screen_rows = rows;
    r->screen_cols = cols;
    r->color       = color;

    /* 落盘失败必须可见：此前调用方一律忽略返回值，现场只表现为
     * "改了显示、重启回到旧内容"，无从查起。 */
    uint32_t len = (uint32_t)sizeof(app_render_persist_t) + bm_bytes;
    if (app_cfg_sched_save(s_persist_id, s_persist_buf, (uint16_t)len) != 0)
        printf("[render] 显存持久化失败（%u 字节）\n", (unsigned)len);

    _persist_unlock();
}

bool app_render_restore(void)
{
    /* 同 save：有画布就整体委托。这条尤其要紧 —— 不委托时会出现"主卡自己的带
       恢复了旧内容、画布却是黑的"，上电后第一轮静默提交会把从卡全刷黑。 */
    if (s_persist_hook && s_persist_hook->restore) return s_persist_hook->restore();

    dev_display_t *d = s_render_display;
    if (!d || s_persist_id == 0xFF)
        return false;

    uint16_t rows     = d->screen_rows;
    uint16_t cols     = d->screen_cols;
    uint16_t bm_bytes = _persist_bm_bytes(d);
    if (!bm_bytes) return false;

    /* 传入的容量取"本屏实际需要"：记录里的 len 与之不符会被 dev_cfg_record_load
     * 判为 INVALID —— 这正是我们要的（换了模组则旧显存不适用）。 */
    uint16_t payload_cap = (uint16_t)(sizeof(app_render_persist_t) + bm_bytes);
    uint16_t rec_len     = 0;

    /* 与 save 共用同一个组装缓冲，故同样要持锁 */
    _persist_lock();

    if (app_cfg_sched_load(s_persist_id, s_persist_buf, payload_cap, &rec_len) != DEV_CFG_RECORD_STATE_OK)
        goto fail;
    if (rec_len != payload_cap) goto fail;

    app_render_persist_t *r = (app_render_persist_t *)s_persist_buf;
    /* 几何已由 len 比对隐含校验（len 由本屏几何算出），这里再核一次字段，
     * 防止"长度碰巧相同但内容不是本屏"的情况。 */
    if (r->screen_rows != rows || r->screen_cols != cols) goto fail;

    dev_display_fill(d, 0, 0, rows, cols, DEV_DISPLAY_COLOR_BLACK);
    dev_display_draw_bitmap(d, 0, 0, rows, cols, r->bitmap, (dev_display_color_t)r->color);
    _persist_unlock();
    return true;

fail:
    _persist_unlock();
    return false;
}
