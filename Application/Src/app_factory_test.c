/**
 * @file    app_factory_test.c
 * @brief   出厂检测模式 — monitor 监听 TEST 驱动状态机
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
#include "pl_task.h"

#define AGING_TEXT   "重庆创迪科技发展有限公司设备老化测试"
#define PROGRAM_CODE "9210209C41"

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

/** 工厂测试是否正在进行（IDLE 之外的所有阶段）。
 *  置位/清零都在 factory_monitor_task 内，app_factory_mode_interrupt 只读它 ——
 *  用它把"每收一包数据"的路径挡在 osThreadTerminate 之外。 */
static volatile bool s_factory_active;

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
        /* IDLE: 等待 TEST 激活 */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);

        s_factory_active = true;

        /* ===== SHOW_CODE ===== */
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = dsp->screen_rows,
            .h         = dsp->screen_cols,
            .color     = COLOR_GREEN,
            .text      = PROGRAM_CODE,
            .len       = strlen(PROGRAM_CODE),
            .font_size = FONT_16,
            .font_type = FONT_ST,
            .text_enc  = FONT_ENC_UTF8,
            .style     = &(render_style_t){
                .h_align = ALIGN_CENTER,
                .v_align = ALIGN_CENTER,
            },
        });

        dev_key_wait_press(DEV_KEY_TST, osWaitForever);

        /* ===== DEAD_PIXEL ===== */
        osThreadSuspend(g_light_sensor_task_handle);
        dev_display_set_brightness(dsp, 7);
        for (uint8_t i = 0; i < DEAD_PIXEL_COLOR_COUNT; i++) {
            dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, s_dead_pixel_colors[i]);
            dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        }

        /* ===== AGING ===== */
        osThreadResume(g_light_sensor_task_handle);

        /* 进衰老轮播前排空残留的信号量令牌。
           上面 DEAD_PIXEL 段用的是 osWaitForever，若那几次按键有抖动多释放了一次
           （信号量上限 1，去抖在 dev_key 的 EXTI 回调里，但历史遗留的窗口仍在），
           余下的令牌会被下面第一次 wait_press(…, 3000) 立刻消费 —— 表现为
           "只显示第一个字就退出轮播并清屏"。这里做最后一道保险。 */
        while (dev_key_wait_press(DEV_KEY_TST, 0)) {}

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

        /* 退出工厂模式 */
        s_factory_active = false;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
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
    g_factory_test = pl_task_new(factory_monitor_task, NULL, &attr);
}

static void _factory_module_init(void)
{
    /* 框架不再直接认识本模块 —— 收到数据就退出工厂模式这件事，
       由本模块自己注册监听（见 app_dispatch_register_rx_listener）。 */
    app_dispatch_register_rx_listener(app_factory_mode_interrupt);
    _factory_test_init();
}
sw_app_initcall(_factory_module_init);

/**
 * @brief 退出工厂测试模式，回到 IDLE
 *
 * **仅在测试进行中才动线程**。此前无条件 osThreadTerminate + osThreadNew，
 * 而本函数被挂在"每收到一包数据"的路径上 —— 有持续 TCP 流量时就是每包一次的
 * 线程创建销毁，对 32KB 的 heap_4 持续碎片化。
 */
void app_factory_mode_interrupt(void)
{
    if (!s_factory_active) return; /* 已在 IDLE：无可中断 */

    s_factory_active = false;
    osThreadTerminate(g_factory_test);

    /* 终止点可能正好落在"挂起光传感器任务"与"恢复"之间（见 DEAD_PIXEL 段），
       那样光传感器就永久挂起了。补一次恢复 —— 对未挂起的线程是空操作。 */
    osThreadResume(g_light_sensor_task_handle);

    _factory_test_init();
}
