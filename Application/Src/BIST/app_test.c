/**
 * @file    app_test.c
 * @brief   硬件测试用例实现
 */

#include "app_test.h"
#include "app_screen.h"

#include <stdio.h>
#include <string.h>
#include "cmsis_os2.h"
#include "dev_display.h"
#include "app_render.h"

/* ================================================================
 *  像素扫描测试
 * ================================================================ */

void app_test_pixel_scan(void)
{
    dev_display_t *dsp = dev_display_get();

    for (;;) {
        for (int i = 0; i < (int)dsp->buffer_size; i++) {
            dev_display_set_pixel(dsp,
                                  i % dsp->screen_rows, /* 逐行 x */
                                  i / dsp->screen_rows, /* 逐列 y */
                                  DEV_DISPLAY_COLOR_GREEN);
            osDelay(500);
            dev_display_set_pixel(dsp,
                                  i % dsp->screen_rows,
                                  i / dsp->screen_rows,
                                  DEV_DISPLAY_COLOR_RED);
        }
    }
}

/* ================================================================
 *  渲染测试
 * ================================================================ */

void app_test_render_text(void)
{
    app_render(&(app_render_cfg_t){
        .type  = APP_RENDER_TYPE_TEXT,
        .x     = 0,
        .y     = 0,
        .w     = app_screen_rows(),
        .h     = app_screen_cols(),
        .style = &(app_render_style_t){
            .h_align = APP_RENDER_ALIGN_CENTER,
            .v_align = APP_RENDER_ALIGN_CENTER,
        },
        .color     = DEV_DISPLAY_COLOR_GREEN,
        .text      = "测试",
        .len       = strlen("测试"),
        .font_size = APP_FONT_SIZE_24,
        .font_type = APP_FONT_TYPE_HT,
        .text_enc  = APP_FONT_ENC_UTF8,
    });
}

/* ================================================================
 *  IO 输出测试
 * ================================================================ */

/* ================================================================
 *  LED 灯序映射测试（48×24 / 1/4 扫）
 *
 *  绕过 pixel_map，直接按**物理移位顺序**写 hub75_buff 逐位点亮：
 *    idx = (line * scan_line_pixels + step) * total_channels + ch
 *  用于确认陌生模组的物理 LED 排列，推导 prepare 的映射规则。
 *
 *  行地址切换与亮度 PWM 仍由显示引擎负责（_scan_task + TIM4 中断），
 *  本测试只改数据缓冲，不碰 A/B/OE/CLK。
 *
 *  注意：ch1（R2G2B2）只有 8 行，每个扫描线只用到前 96 个 step；
 *  后半段即使点亮也不会出现在可见像素上，属正常现象。
 * ================================================================ */

void app_test_led_mapping(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    /* 走模组的 test 钩子：`_scan` 忽略 hub75_buff，只在 (step, ch) 输出一个绿点。
       地址切换、LAT 时序、亮度 PWM 全由显示引擎负责 —— 不碰 CLK/LAT/A/B/OE，
       也就不会和 _scan_task / TIM4 抢总线。 */
    for (;;) {
        for (uint8_t ch = 0; ch < dsp->total_channels; ch++) {
            /* ch1（R2G2B2）只有 8 行 = 96 步有效，step ≥ 96 不会点亮，属正常 */
            for (uint16_t step = 0; step < dsp->scan_line_pixels; step++) {
                dev_display_set_test_point(dsp, (int16_t)step, ch);
                printf("[map] ch=%u step=%u\n", (unsigned)ch, (unsigned)step);
                osDelay(300);
            }
        }
        dev_display_set_test_point(dsp, -1, 0);
        osDelay(1000);
    }
}

/* ================================================================
 *  定点灯序测试
 *
 *  只点亮指定的几个移位位置，每个停 5 秒，用于核对"某个 index 落在屏幕哪里"。
 *  顺序固定，按顺序报坐标即可；RTT 同时打印 [map] idx=N。
 *  每次先把 test_step 置 -1 清掉红色轨迹，保证屏上只有一个点。
 * ================================================================ */

void app_test_led_points(void)
{
    static const uint16_t pts[] = {16U, 32U, 64U, 96U, 128U, 160U, 191U};
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    for (;;) {
        for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
            dev_display_set_test_point(dsp, -1, 0); /* 清轨迹 */
            osDelay(50);
            printf("[map] idx=%u\n", (unsigned)pts[i]);
            dev_display_set_test_point(dsp, (int16_t)pts[i], 0);
            osDelay(5000);
        }
    }
}

/* ================================================================
 *  映射校验图
 * ================================================================ */

void app_test_map_check(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    const uint16_t w = dsp->screen_rows; /* 宽 */
    const uint16_t h = dsp->screen_cols; /* 高 */

    dev_display_frame_begin(dsp);
    dev_display_fill(dsp, 0, 0, w, h, DEV_DISPLAY_COLOR_BLACK);

    /* 绿色边框（1 像素） */
    for (uint16_t x = 0; x < w; x++) {
        dev_display_set_pixel(dsp, x, 0, DEV_DISPLAY_COLOR_GREEN);
        dev_display_set_pixel(dsp, x, (uint16_t)(h - 1U), DEV_DISPLAY_COLOR_GREEN);
    }
    for (uint16_t y = 0; y < h; y++) {
        dev_display_set_pixel(dsp, 0, y, DEV_DISPLAY_COLOR_GREEN);
        dev_display_set_pixel(dsp, (uint16_t)(w - 1U), y, DEV_DISPLAY_COLOR_GREEN);
    }

    /* 左上 4×4 红色块：镜像/旋转时它会跑到别的角，一眼可辨 */
    for (uint16_t y = 0; y < 4U; y++)
        for (uint16_t x = 0; x < 4U; x++) dev_display_set_pixel(dsp, x, y, DEV_DISPLAY_COLOR_RED);

    /* 从左上到右下的红色对角线：方向错了一眼可见 */
    for (uint16_t x = 0; x < w; x++)
        dev_display_set_pixel(dsp, x, (uint16_t)((uint32_t)x * (h - 1U) / (w - 1U)), DEV_DISPLAY_COLOR_RED);

    /* 右下 2×2 红色块，进一步确认对角 */
    for (uint16_t y = (uint16_t)(h - 2U); y < h; y++)
        for (uint16_t x = (uint16_t)(w - 2U); x < w; x++) dev_display_set_pixel(dsp, x, y, DEV_DISPLAY_COLOR_RED);

    /* 中线横条（绿），确认行方向 */
    for (uint16_t x = 0; x < w; x++) dev_display_set_pixel(dsp, x, (uint16_t)(h / 2U), DEV_DISPLAY_COLOR_GREEN);

    dev_display_frame_end(dsp);

    for (;;) osDelay(1000);
}

/* ================================================================
 *  测试入口
 * ================================================================ */

void app_test_run(void)
{
    app_test_render_text(); /* 16×16 绿色黑体"测试"，居中 */
    // app_test_map_check();
    // app_test_led_points();
    // app_test_led_mapping();
    // app_test_pixel_scan();
    // board_test_io_output(); /* 板级测试，见 boards/&lt;板&gt;/Application/Src/app_test_board.c */
}
