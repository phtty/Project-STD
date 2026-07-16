/**
 * @file    app_test.c
 * @brief   硬件测试用例实现
 */

#include "app_test.h"

#include <string.h>
#include "cmsis_os2.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "app_render.h"

/* ================================================================
 *  像素扫描测试
 * ================================================================ */

void app_test_pixel_scan(void)
{
    dev_display_t *dsp = dev_display_p20_get();

    for (;;) {
        for (int i = 0; i < (int)dsp->buffer_size; i++) {
            dev_display_set_pixel(dsp,
                                  i % dsp->screen_cols, /* 逐列 x */
                                  i / dsp->screen_cols, /* 逐行 y */
                                  COLOR_GREEN);
            osDelay(50);
            dev_display_set_pixel(dsp,
                                  i % dsp->screen_cols,
                                  i / dsp->screen_cols,
                                  COLOR_BLACK);
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
            .h_align = ALIGN_CENTER,
            .v_align = ALIGN_CENTER,
        },
        .color     = COLOR_RED,
        .text      = "车道关闭",
        .len       = strlen("车道关闭"),
        .font_size = FONT_32,
        .font_type = FONT_ST,
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
 *  绕过 pixel_map，直接写入 hub75_buff 逐位置点亮。
 *  用于确认陌生模组的物理 LED 排列顺序，推导 prepare 映射规则。
 *  按通道分组，每通道从第 0 像素起依次亮 200ms 红灯后熄灭。
 * ================================================================ */

void app_test_led_mapping(void)
{
    dev_display_t *dsp = dev_display_p20_get();

    /* 清空 hub75_buff，确保 prepare 不会覆盖（dirty=false） */
    memset(dsp->hub75_buff, 0, dsp->buffer_size);
    dsp->dirty = false;

    for (uint8_t ch = 0; ch < dsp->total_channels; ch++) {
        for (uint16_t px = 0; px < dsp->channel_pixels; px++) {
            uint16_t pos = px + ch * dsp->channel_pixels; /* hub75_buff 线性位置 */

            /* 点亮当前像素（红色醒目） */
            dsp->hub75_buff[pos] = COLOR_RED;
            osDelay(200);
        }
        /* 通道切换停顿，方便标记 */
        osDelay(500);
    }

    for (;;); // 暂停运行
}

/* ================================================================
 *  测试入口
 * ================================================================ */

void app_test_run(void)
{
    // app_test_led_mapping(); /* 新模组灯序确认时启用 */
    // app_test_pixel_scan();
    app_test_render_text();
    // app_test_io_output();
}
