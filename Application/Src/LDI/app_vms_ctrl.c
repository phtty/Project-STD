#include "app_vms_ctrl.h"

#include "string.h"

#include "app_render.h"

/* ---- VMS 定时清屏 ---- */
static uint32_t s_vms_clear_tick; /* 清屏时刻 (RTOS tick) */
static bool     s_vms_timer_active; /* 定时器是否激活 */

static void vms_clear_screen(void)
{
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = COLOR_BLACK,
    });
    app_render_save();
}

void vms_timer_poll(void)
{
    if (!s_vms_timer_active) return;

    if (osKernelGetTickCount() >= s_vms_clear_tick) {
        vms_clear_screen();
        s_vms_timer_active = false;
    }
}

/* ================================================================
 *  查表映射 — LDI 协议编号 → 渲染引擎枚举
 * ================================================================ */

/** LDI 字体颜色 → display_color_t (01H=绿, 02H=红, 03H=黄, 其他=黑) */
static const display_color_t s_color_map[] = {
    [1] = COLOR_GREEN,
    [2] = COLOR_RED,
    [3] = COLOR_YELLOW,
};

/** LDI 对齐方式 format → align_t (01H=居中, 02H=左, 03H=右) */
static const align_t s_align_map[] = {
    [1] = ALIGN_CENTER,
    [2] = ALIGN_LEFT_UP,
    [3] = ALIGN_RIGHT_DOWN,
};

/** LDI 字号 font_size → font_size_t (00H=自适应, 01H=16, 02H=24, 03H=32) */
static const font_size_t s_font_size_map[] = {
    [0] = FONT_SELF_ADAPT,
    [1] = FONT_16,
    [2] = FONT_24,
    [3] = FONT_32,
};

/** LDI 清屏颜色 clear_type → display_color_t (00H=黑, 01H=红, 02H=绿, 03H=黄) */
static const display_color_t s_clear_color_map[] = {
    [0] = COLOR_BLACK,
    [1] = COLOR_RED,
    [2] = COLOR_GREEN,
    [3] = COLOR_YELLOW,
};

/* ---- 带边界检查的查表辅助宏 ---- */
#define MAP(table, idx, fallback) \
    ((idx) < (sizeof(table) / sizeof((table)[0])) ? (table)[(idx)] : (fallback))

/* ================================================================
 *  显示控制 (func_type = 0x01)
 * ================================================================ */

static void vms_display_ctrl(ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    display_color_t color     = MAP(s_color_map, ctx->font_color, COLOR_BLACK);
    align_t         h_align   = MAP(s_align_map, ctx->format, ALIGN_CENTER);
    font_size_t     font_size = MAP(s_font_size_map, ctx->font_size, FONT_16);

    dev_display_t *d  = dev_display_get();
    uint16_t screen_w = d->screen_rows;
    uint16_t screen_h = d->screen_cols;

    /* ---- 计算行布局 ---- */
    uint16_t render_y = 0;
    uint16_t render_h = screen_h;
    align_t  v_align  = ALIGN_CENTER;

    /* font_line > 0: 将屏幕按字号划分为若干行，文字显示在指定行；
       font_line == 0: 上下居中 (默认) */
    if (ctx->font_line > 0) {
        /* 自适应字号时以 16 为基准计算行高 */
        uint16_t line_height = (font_size == FONT_SELF_ADAPT) ? 16U : (uint16_t)font_size;
        uint16_t total_lines = screen_h / line_height;

        if (total_lines > 0 && ctx->font_line <= total_lines) {
            render_y = (ctx->font_line - 1) * line_height;
            render_h = line_height;
            v_align  = ALIGN_LEFT_UP; /* 单行内不居中 */

            /* 强制使用具体字号，否则渲染器会自适应缩放 */
            if (font_size == FONT_SELF_ADAPT) font_size = FONT_16;
        }
        /* font_line 超出范围 → 回退到居中模式 */
    }

    /* ---- 构建渲染风格 ---- */
    render_style_t style = {
        .h_align   = h_align,
        .v_align   = v_align,
        .word_wrap = false,
    };

    /* ---- LDI 协议 '_' → 换行 ---- */
    for (uint16_t i = 0; i < text_len; i++)
        if (ctx->text[i] == '_')
            ctx->text[i] = '\n';

    /* ---- 清屏 ---- */
    vms_clear_screen();

    /* ---- 渲染文字 ---- */
    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = render_y,
        .w         = screen_w,
        .h         = render_h,
        .color     = color,
        .text      = (char *)ctx->text,
        .len       = text_len,
        .style     = &style,
        .font_size = font_size,
        .font_type = FONT_HT,
        .text_enc  = FONT_ENC_GBK,
    });

    /* ---- 持久化策略 ---- */
    if (ctx->keep_time == 0) {
        /* 永久显示：存入 Flash */
        app_render_save();
        s_vms_timer_active = false;
    } else {
        /* 定时显示：keep_time 秒后自动清屏 */
        s_vms_clear_tick   = osKernelGetTickCount() + (uint32_t)ctx->keep_time * 1000U;
        s_vms_timer_active = true;
    }
}

/* ================================================================
 *  清屏控制 (func_type = 0x02)
 * ================================================================ */

static void vms_clean_ctrl(ldi_ctrl_vms_t *ctx)
{
    display_color_t color = MAP(s_clear_color_map, ctx->clear_type, COLOR_BLACK);

    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .w     = 0,
        .h     = 0,
        .color = color,
    });

    /* 主动清屏时取消定时器 */
    s_vms_timer_active = false;
}

/* ---- 公共入口 ---- */
void vms_ctrl(ldi_ctrl_vms_t *ctx, const uint16_t text_len)
{
    if (ctx->device_func_type == 0x01)
        vms_display_ctrl(ctx, text_len);
    else if (ctx->device_func_type == 0x02)
        vms_clean_ctrl(ctx);
}
