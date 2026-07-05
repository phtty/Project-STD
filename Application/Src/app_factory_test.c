/**
 * @file    app_factory_test.c
 * @brief   出厂检测模式实现 — FreeRTOS 任务挂起/恢复隔离
 */

#include "app_factory_test.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"
#include "dev_key.h"
#include "app_render.h"
#include "app_dispatch.h"
#include "app_light_sensor.h"

#define AGING_TEXT       "重庆创迪科技发展有限公司设备老化测试"
#define AGING_CHAR_COUNT (sizeof(AGING_TEXT) - 1) /* 17 */
#define PROGRAM_CODE     "9210209840"

/* ---- 坏点检测颜色顺序 ---- */
static const display_color_t s_dead_pixel_colors[] = {
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_PURPLE,
    COLOR_WHITE,
};
#define DEAD_PIXEL_COLOR_COUNT (sizeof(s_dead_pixel_colors) / sizeof(s_dead_pixel_colors[0]))

/* ---- 字号/字体轮询顺序 ---- */
static const font_size_t s_aging_sizes[] = {FONT_14, FONT_16, FONT_20, FONT_24, FONT_32};
static const font_type_t s_aging_types[] = {FONT_ST, FONT_FS, FONT_KT, FONT_HT};
#define AGING_SIZE_COUNT (sizeof(s_aging_sizes) / sizeof(s_aging_sizes[0]))
#define AGING_TYPE_COUNT (sizeof(s_aging_types) / sizeof(s_aging_types[0]))

/* ---- 状态机 ---- */
typedef enum {
    FACTORY_IDLE = 0,
    FACTORY_SHOW_CODE,
    FACTORY_DEAD_PIXEL,
    FACTORY_AGING,
} factory_state_t;

/* ---- 调试变量 ---- */
volatile int g_factory_state_debug; /* 当前阶段 */
volatile int g_factory_color_idx;   /* 坏点检测颜色索引 */
volatile int g_factory_aging_type;  /* 老化当前字体索引 */
volatile int g_factory_aging_size;  /* 老化当前字号索引 */
volatile int g_factory_aging_char;  /* 老化当前字符索引 */

/* ---- 进入/退出工厂模式 ---- */
static void _factory_enter(void)
{
    osThreadSuspend(g_dispatch_task_handle);     /* 切断协议流水线 */
    osThreadSuspend(g_light_sensor_task_handle); /* 锁亮度 */
}

static void _factory_exit(void)
{
    dev_display_fill(dev_display_get(), COLOR_BLACK);
    osThreadResume(g_light_sensor_task_handle); /* 恢复自动调光 */
    osThreadResume(g_dispatch_task_handle);     /* 恢复协议 */
}

/* ---- 老化模式：铺满全屏 N 份字符 ---- */
static void _aging_fill_screen(font_size_t size, font_type_t type, const char *ch_utf8, uint8_t ch_len)
{
    dev_display_t *dsp = dev_display_get();
    uint8_t glyph_w    = size; /* 老化文本中文全宽 */
    uint8_t glyph_h    = size;

    dev_display_fill(dsp, COLOR_BLACK);

    uint8_t cols = dsp->screen_cols / glyph_w;
    uint8_t rows = dsp->screen_rows / glyph_h;
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    for (uint8_t row = 0; row < rows; row++) {
        for (uint8_t col = 0; col < cols; col++) {
            app_render(&(render_cfg_t){
                .type      = RENDER_TEXT,
                .x         = col * glyph_w,
                .y         = row * glyph_h,
                .w         = glyph_w,
                .h         = glyph_h,
                .color     = COLOR_RED,
                .text      = ch_utf8,
                .len       = ch_len,
                .font_size = size,
                .font_type = type,
                .text_enc  = FONT_ENC_UTF8,
            });
        }
    }
}

/* ================================================================
 *  主任务
 * ================================================================ */

void app_factory_test_task(void *argument)
{
    (void)argument;
    dev_display_t *dsp = dev_display_get();
    uint8_t color_idx;

    for (;;) {
        /* 等待 TEST 按键 */
        while (!dev_key_test_pressed())
            osDelay(50);

        /* ===== 进入工厂模式 ===== */
        _factory_enter();
        dev_display_set_brightness(dsp, 7);
        g_factory_state_debug = FACTORY_SHOW_CODE;

        /* ===== SHOW_CODE ===== */
        dev_display_fill(dsp, COLOR_BLACK);
        render_style_t style = {.h_align = ALIGN_CENTER, .v_align = ALIGN_CENTER};
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = dsp->screen_cols,
            .h         = dsp->screen_rows,
            .color     = COLOR_RED,
            .text      = PROGRAM_CODE,
            .len       = strlen(PROGRAM_CODE),
            .font_size = FONT_16,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_ASCII,
            .style     = &style,
        });

        while (!dev_key_test_pressed())
            osDelay(50);

        /* ===== DEAD_PIXEL ===== */
        g_factory_state_debug = FACTORY_DEAD_PIXEL;
        color_idx             = 0;

        for (;;) {
            g_factory_color_idx = color_idx;
            dev_display_fill(dsp, s_dead_pixel_colors[color_idx]);

            while (!dev_key_test_pressed())
                osDelay(50);

            color_idx++;
            if (color_idx >= DEAD_PIXEL_COLOR_COUNT)
                break;
        }

        /* ===== AGING ===== */
        g_factory_state_debug = FACTORY_AGING;
        osThreadResume(g_light_sensor_task_handle); /* 恢复自动调光 */

        for (uint8_t type_idx = 0;; type_idx = (type_idx + 1) % AGING_TYPE_COUNT) {
            g_factory_aging_type = type_idx;
            font_type_t ftype    = s_aging_types[type_idx];

            for (uint8_t size_idx = 0; size_idx < AGING_SIZE_COUNT; size_idx++) {
                g_factory_aging_size = size_idx;
                font_size_t fsize    = s_aging_sizes[size_idx];

                if (fsize > dsp->screen_rows || fsize > dsp->screen_cols)
                    continue;

                const char *ch_ptr = AGING_TEXT;
                while (*ch_ptr) {
                    /* UTF-8 单字: 中文 BMP 字符固定 3 字节 */
                    uint8_t ch_len    = ((uint8_t)*ch_ptr >= 0xE0) ? 3 : 1;
                    char single_ch[4] = {ch_ptr[0], ch_len > 1 ? ch_ptr[1] : 0,
                                         ch_len > 2 ? ch_ptr[2] : 0, 0};
                    _aging_fill_screen(fsize, ftype, single_ch, ch_len);

                    ch_ptr += ch_len;

                    uint32_t deadline = osKernelGetTickCount() + 3000;
                    while ((int32_t)(osKernelGetTickCount() - deadline) < 0) {
                        if (dev_key_test_pressed())
                            goto factory_exit;
                        osDelay(50);
                    }
                }
            }
        }

    factory_exit:
        _factory_exit();
        g_factory_state_debug = FACTORY_IDLE;
    }
}

/* ---- 模块自注册 ---- */
static void _factory_test_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "factory_test",
        .stack_size = 512 * 4,
        .priority   = osPriorityHigh,
    };
    osThreadNew(app_factory_test_task, NULL, &attr);
}
sw_app_initcall(_factory_test_init);
