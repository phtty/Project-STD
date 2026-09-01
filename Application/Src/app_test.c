/**
 * @file    app_test.c
 * @brief   硬件测试用例实现
 */

#include "app_test.h"

#include <string.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
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
                                  i % dsp->screen_rows, /* 逐列 x */
                                  i / dsp->screen_rows, /* 逐行 y */
                                  COLOR_GREEN);
            dev_display_commit_frame(dsp);
            osDelay(50);
            dev_display_set_pixel(dsp,
                                  i % dsp->screen_rows,
                                  i / dsp->screen_rows,
                                  COLOR_BLACK);
            dev_display_commit_frame(dsp);
        }
    }
}

/* ================================================================
 *  渲染测试
 * ================================================================ */

void app_test_render_text(void)
{
    app_render(&(render_cfg_t){
        .type  = RENDER_TEXT,
        .x     = 0,
        .y     = 0,
        .w     = dev_display_get()->screen_rows,
        .h     = dev_display_get()->screen_cols,
        .style = &(render_style_t){
            .h_align   = ALIGN_LEFT_UP,
            .v_align   = ALIGN_LEFT_UP,
            .word_wrap = false,
        },
        .color     = COLOR_YELLOW,
        .text      = "车道",
        .len       = strlen("车道"),
        .font_size = FONT_SELF_ADAPT,
        .font_type = FONT_HT,
        .text_enc  = FONT_ENC_UTF8,
    });
}

/* ================================================================
 *  IO 输出测试
 * ================================================================ */

void app_test_io_output(void)
{
    dev_io_lane_light(true);
    osDelay(1000);
    dev_io_lane_light(false);

    dev_io_flash_light(true);
    osDelay(1000);
    dev_io_flash_light(false);
}

/* ================================================================
 *  LED 灯序映射测试
 *
 *  绕过 pixel_map，直接按 hub75_buff 线性顺序逐位置点亮。
 *  屏幕上像素的出现顺序即为 hub75_buff 的物理输出顺序，
 *  用于确认陌生模组的 LED 排列规律，推导 prepare 映射规则。
 *  已点亮的像素不熄灭，逐个填充整个缓冲区。
 *
 *  PASS 标准：
 *  1. 每次新点亮的像素为纯红色，无偏色/混色
 *  2. 像素按顺序逐个填充，无跳过或乱序
 *
 *  通过后可确保：
 *  - _1_260_scan() 时钟脉冲和数据输出时序正确
 *  - g_bsrr[color] 查表输出 R/G/B 颜色正确
 *  - pl_hub75_ShiftRegister_set_row() 行选通正确
 *  - HUB75 物理连线（排线、通道引脚）正确
 *
 *  无法确保：
 *  - _1_260_prepare() 的像素重排逻辑（本测试绕过 prepare）
 *  - pixel_map 坐标映射
 * ================================================================ */

void app_test_led_mapping(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    memset(dsp->hub75_buff, 0, dsp->buffer_size);
    dsp->dirty = false;

    for (uint16_t pos = 0; pos < dsp->buffer_size; pos++) {
        dsp->hub75_buff[pos] = COLOR_RED;
        osDelay(100);
    }

    for (;;);
}

/* ================================================================
 *  扫描行顺序测试
 *
 *  逐行点亮水平线，用于确认 1/8 扫描模组的行地址映射顺序。
 *  每条线显示 1 秒后切换到下一行。
 * ================================================================ */

void app_test_scan_line_order(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    for (uint8_t line = 0; line < 8; line++) {
        /* 清屏 */
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        dev_display_commit_frame(dsp);
        osDelay(100);

        /* 在第 line 行画一条水平红线 */
        for (uint16_t x = 0; x < dsp->screen_rows; x++) {
            dev_display_set_pixel(dsp, x, line, COLOR_RED);
        }
        dev_display_commit_frame(dsp);

        /* 保持显示 1 秒 */
        osDelay(1000);
    }

    /* 测试完成后清屏 */
    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(dsp);
}

/* ================================================================
 *  对角线测试
 *
 *  画一条从左上到右下的对角线，用于验证整体坐标映射是否正确。
 * ================================================================ */

void app_test_diagonal(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(dsp);
    osDelay(100);

    uint16_t size = (dsp->screen_rows < dsp->screen_cols) ? dsp->screen_rows : dsp->screen_cols;
    for (uint16_t i = 0; i < size; i++) {
        dev_display_set_pixel(dsp, i, i, COLOR_GREEN);
    }
    dev_display_commit_frame(dsp);

    osDelay(3000);
    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    dev_display_commit_frame(dsp);
}

/* ================================================================
 *  斜扫测试
 * ================================================================ */

volatile uint32_t g_app_test_oblique_frame_count;

void app_test_oblique_scan_frame(dev_display_t *dsp, int16_t phase)
{
    if (!dsp)
        return;

    const int16_t first_line = -(int16_t)dsp->screen_cols;
    const int16_t last_line  = (int16_t)dsp->screen_rows + (int16_t)dsp->screen_cols - 2;
    const uint16_t line_count =
        (uint16_t)((last_line - first_line) / APP_TEST_OBLIQUE_LINE_SPACING) + 1;

    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);

    for (uint16_t n = 0; n < line_count; n++) {
        int32_t diagonal = (int32_t)first_line + phase +
                           (int32_t)n * APP_TEST_OBLIQUE_LINE_SPACING;

        for (uint16_t y = 0; y < dsp->screen_cols; y++) {
            int32_t x = diagonal - (int32_t)y;
            if (x >= 0 && x < (int32_t)dsp->screen_rows)
                dev_display_set_pixel(dsp, (uint16_t)x, y, COLOR_WHITE);
        }
    }

    g_app_test_oblique_frame_count++;
    dev_display_commit_frame(dsp);
}

void app_test_oblique_scan(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    for (;;) {
        for (int16_t phase = 0; phase < (int16_t)APP_TEST_OBLIQUE_LINE_SPACING; phase++) {
            app_test_oblique_scan_frame(dsp, phase);
            osDelay(APP_TEST_OBLIQUE_STEP_DELAY_MS);
        }
    }
}

/* ================================================================
 *  prepare 映射测试
 *
 *  通过 set_pixel 按屏幕坐标（左上→右下）逐像素点亮，
 *  走完 set_pixel → prepare → scan 全链路。
 *  观察像素是否从模组左上角依次填充整个屏幕。
 *
 *  PASS 标准：
 *  1. 像素从左上角 (0,0) 开始，逐列→逐行填充整个屏幕
 *  2. 每次新点亮的像素为纯红色，无偏色/混色
 *  3. 已点亮的像素不熄灭，逐个填充
 *
 *  通过后可确保：
 *  - _1_260_prepare() 像素重排逻辑正确
 *
 *  无法确保：
 *  - scan 输出时序、行选通、物理连线（由 app_test_led_mapping 验证）
 * ================================================================ */

void app_test_prepare_mapping(void)
{
    dev_display_t *dsp = dev_display_get();
    if (!dsp) return;

    for (uint16_t y = 0; y < dsp->screen_cols; y++) {
        for (uint16_t x = 0; x < dsp->screen_rows; x++) {
            dev_display_set_pixel(dsp, x, y, COLOR_RED);
            dev_display_commit_frame(dsp);
            osDelay(100);
        }
    }

    for (;;);
}

/* ================================================================
 *  测试入口
 * ================================================================ */

void app_test_run(void)
{
    app_test_oblique_scan();
    // app_test_led_mapping();       /* 直接写 hub75_buff，按 hub75_buff线形点亮LED ，测试物理 LED 灯序映射 */
    // app_test_prepare_mapping();   /* 逐行点亮像素，测试 prepare 映射是否正确 */
    // app_test_pixel_scan();        /* 逐行素点亮再熄灭，测试屏幕所有像素点是否正常 */
    // app_test_render_text();       /* 渲染文字"车道关闭"，测试字库读取和文字渲染功能 */
    // app_test_scan_line_order();   /* 逐行点亮水平线，测试 1/8 扫描的行地址映射顺序 */
    // app_test_diagonal();          /* 画对角线，测试整体坐标映射是否正确（直线=正确） */
    // app_test_io_output();         /* 依次点亮车道灯和黄闪灯各 1 秒，测试 IO 输出功能 */
}
