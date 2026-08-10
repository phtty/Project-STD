/**
 * @file    app_factory_test.c
 * @brief   出厂检测模式 — monitor 监听 TEST 驱动状态机
 *
 * 按键测试阶段（SHOW_CODE / RED / GREEN / YELLOW）：光敏自动调光生效。
 * 老化循环阶段（AGING）：强制最大亮度，光敏暂停。
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
#include "dev_io_ctrl.h"
#include "stm32f4xx_hal.h"

#define AGING_TEXT   "重庆创迪科技发展有限公司设备老化测试"
#define PROGRAM_CODE "9K10212482"

static const display_color_t s_dead_pixel_colors[] = {
    COLOR_RED,
    COLOR_GREEN,
    COLOR_YELLOW,
};
#define DEAD_PIXEL_COLOR_COUNT (sizeof(s_dead_pixel_colors) / sizeof(s_dead_pixel_colors[0]))

static const font_size_t s_aging_sizes[] = {
    FONT_16,
    FONT_24,
    FONT_32,
};
static const font_type_t s_aging_types[] = {
    FONT_ST,
    FONT_FS,
    FONT_KT,
    FONT_HT,
};
#define AGING_SIZE_COUNT (sizeof(s_aging_sizes) / sizeof(s_aging_sizes[0]))
#define AGING_TYPE_COUNT (sizeof(s_aging_types) / sizeof(s_aging_types[0]))

osThreadId_t g_factory_test;

/* ---- 老化辅助 ---- */
static void _aging_fill_screen(font_size_t size, font_type_t type, const char *ch_utf8, uint8_t ch_len)
{
    dev_display_t *dsp = dev_display_get();

    uint8_t cols = dsp->screen_cols / size;
    uint8_t rows = dsp->screen_rows / size;
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    dsp->dirty = false; /* 防止 scan 在 fill 和 render 之间输出全黑帧 */

    /* 把字符重复 cols×rows 份放缓冲区，word_wrap 自动分行 */
    static char buf[256];
    uint16_t pos   = 0;
    uint16_t count = cols * rows;
    while (count--) {
        memcpy(buf + pos, ch_utf8, ch_len);
        pos += ch_len;
    }

    app_render(&(render_cfg_t){
        .type      = RENDER_TEXT,
        .x         = 0,
        .y         = 0,
        .w         = dsp->screen_rows,
        .h         = dsp->screen_cols,
        .color     = COLOR_WHITE,
        .text      = buf,
        .len       = pos,
        .font_size = size,
        .font_type = type,
        .text_enc  = FONT_ENC_UTF8,
        .style     = &(render_style_t){
                .h_align   = ALIGN_CENTER,
                .v_align   = ALIGN_CENTER,
                .word_wrap = true,
        },
    });
}

/* ================================================================
 *  factory_monitor_task
 * ================================================================ */

static void factory_monitor_task(void *argument)
{
    (void)argument;
    dev_display_t *dsp = dev_display_get();

    for (;;) {
        /* ===== IDLE: 等待 TEST 激活 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);

        /* ===== 第1次按键 → SHOW_CODE: 显示固件/模组信息 ===== */
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = dsp->screen_rows,
            .h         = dsp->screen_cols,
            .color     = COLOR_GREEN,
            .text      = "FW:" PROGRAM_CODE "\nMD:1000000969",
            .len       = strlen("FW:" PROGRAM_CODE "\nMD:1000000969"),
            .font_size = FONT_SELF_ADAPT,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_UTF8,
            .style     = &(render_style_t){
                    .h_align = ALIGN_CENTER,
                    .v_align = ALIGN_CENTER,
            },
        });

        /* ===== 第2次按键 → RED: 全屏红色 =====   */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_RED);
        dev_display_commit_frame(dsp);

        /* ===== 第3次按键 → GREEN: 全屏绿色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_GREEN);
        dev_display_commit_frame(dsp);

        /* ===== 第4次按键 → YELLOW: 全屏黄色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_YELLOW);
        dev_display_commit_frame(dsp);

        /* ===== 第5次按键 → WHITE: 全屏白色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_WHITE);
        dev_display_commit_frame(dsp);

        /* ===== 第6次按键 → BLUE: 全屏蓝色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLUE);
        dev_display_commit_frame(dsp);

        /* ===== 第7次按键 → PURPLE: 全屏紫色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_PURPLE);
        dev_display_commit_frame(dsp);

        /* ===== 第8次按键 → CYAN: 全屏青色 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_CYAN);
        dev_display_commit_frame(dsp);

        /* ===== 第9次按键 → 通信灯红叉 + 报警灯开启 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = dsp->screen_rows,
            .h         = dsp->screen_cols,
            .color     = COLOR_GREEN,
            .text      = "通信灯红叉\n报警灯开启",
            .len       = strlen("通信灯红叉\n报警灯开启"),
            .font_size = FONT_SELF_ADAPT,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_UTF8,
            .style     = &(render_style_t){
                    .h_align = ALIGN_CENTER,
                    .v_align = ALIGN_CENTER,
            },
        });
        dev_io_lane_light(false);
        dev_io_flash_light(true);
        dev_display_commit_frame(dsp);

        /* ===== 第10次按键 → 通信灯绿箭 + 报警灯关闭 ===== */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = dsp->screen_rows,
            .h         = dsp->screen_cols,
            .color     = COLOR_GREEN,
            .text      = "通信灯绿箭\n报警灯关闭",
            .len       = strlen("通信灯绿箭\n报警灯关闭"),
            .font_size = FONT_SELF_ADAPT,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_UTF8,
            .style     = &(render_style_t){
                    .h_align = ALIGN_CENTER,
                    .v_align = ALIGN_CENTER,
            },
        });
        dev_io_lane_light(true);
        dev_io_flash_light(false);
        dev_display_commit_frame(dsp);

        /* ===== 第11次按键 → AGING: 进入老化循环 =====
         * 暂停光敏，强制最大亮度 */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        osThreadSuspend(g_light_sensor_task_handle);
        dev_display_set_brightness(dsp, 7);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        dev_display_commit_frame(dsp);

        bool aging_exit = false;
        for (uint8_t type_idx = 0; !aging_exit; type_idx = (type_idx + 1) % AGING_TYPE_COUNT) {
            for (uint8_t size_idx = 0; size_idx < AGING_SIZE_COUNT; size_idx++) {
                font_size_t fsize = s_aging_sizes[size_idx];
                if (fsize > dsp->screen_rows || fsize > dsp->screen_cols) continue;

                const char *ch_ptr = AGING_TEXT;
                while (*ch_ptr) {
                    uint8_t ch_len    = ((uint8_t)*ch_ptr >= 0xE0) ? 3 : 1;
                    char single_ch[4] = {ch_ptr[0], ch_len > 1 ? ch_ptr[1] : 0,
                                         ch_len > 2 ? ch_ptr[2] : 0, 0};

                    _aging_fill_screen(fsize, s_aging_types[type_idx], single_ch, ch_len);

                    if (dev_key_wait_press(DEV_KEY_TST, 3000)) {
                        aging_exit = true;
                        break;
                    }
                    ch_ptr += ch_len;
                }
                if (aging_exit) break;
            }
            if (aging_exit) break;
        }

        /* ===== 第12次按键 → 重启程序 ===== */
        NVIC_SystemReset();
    }
}

/* ---- 模块自注册 ---- */
static void _factory_test_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "factory_monitor",
        .stack_size = 512 * 4,
        .priority   = osPriorityBelowNormal,
    };
    g_factory_test = osThreadNew(factory_monitor_task, NULL, &attr);
}
sw_app_initcall(_factory_test_init);

// 对外提供一个终止工厂测试模式的接口
void app_factory_mode_interrupt(void)
{
    // dev_display_fill(dev_display_get(), 0, 0, dev_display_get()->screen_rows, dev_display_get()->screen_cols, COLOR_BLACK);
    osThreadTerminate(g_factory_test);
    _factory_test_init();
}
