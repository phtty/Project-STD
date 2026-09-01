/**
 * @file    app_factory_test.c
 * @brief   出厂检测模式 — monitor 监听 TEST 驱动状态机
 *
 * 按键测试阶段（SHOW_CODE / RED / GREEN / YELLOW）：光敏自动调光生效。
 * 老化循环阶段（AGING）：强制最大亮度，光敏暂停。
 */

#include "app_factory_test.h"

#include <string.h>
#include <stdio.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "dev_display.h"
#include "dev_key.h"
#include "app_render.h"
#include "app_dispatch.h"
#include "app_light_sensor.h"
#include "app_test.h"
#include "dev_io_ctrl.h"
#include "stm32f4xx_hal.h"
#include "app_boot.h"

#define AGING_TEXT   "重庆创迪科技发展有限公司设备老化测试"

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

/* ---- 业务数据中止机制 ----
 * 业务数据（任意通道）到达时 app_factory_mode_interrupt() 置位本标志。
 * monitor 任务不销毁、不回退：各按键等待点分片唤醒检查标志，中止当前
 * 检测序列后回到 IDLE 继续等待 TEST 键。业务数据到达仅中止当前
 * 检测序列，按键扫描持续运行，TEST 键始终可用。 */
static volatile bool s_factory_abort;

/* ---- 工厂测试激活态（供心跳类定时任务查询，暂停计数）---- */
static volatile bool s_factory_active;

bool app_factory_test_active(void)
{
    return s_factory_active;
}

/**
 * @brief  等待 TEST 键按下，支持业务数据中止。
 * @param  timeout_ms  等待时限（osWaitForever 表示永久）。
 * @return true=按键按下；false=超时或业务数据中止。
 */
static bool _factory_wait_tst(uint32_t timeout_ms)
{
    const uint32_t slice = 100U; /* 分片唤醒，响应 s_factory_abort */
    if (timeout_ms == osWaitForever) {
        for (;;) {
            if (dev_key_wait_press(DEV_KEY_TST, slice))
                return true;
            if (s_factory_abort)
                return false;
        }
    }
    while (timeout_ms > 0U) {
        uint32_t t = (timeout_ms > slice) ? slice : timeout_ms;
        if (dev_key_wait_press(DEV_KEY_TST, t))
            return true;
        if (s_factory_abort)
            return false;
        timeout_ms -= t;
    }
    return false;
}

/* ---- 老化辅助 ---- */
static void _aging_fill_screen(font_size_t size, font_type_t type, const char *ch_utf8, uint8_t ch_len)
{
    dev_display_t *dsp = dev_display_get();

    uint8_t cols = dsp->screen_cols / size;
    uint8_t rows = dsp->screen_rows / size;
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
    dsp->dirty = false;

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
    dev_display_commit_frame(dsp);
}

/* ================================================================
 *  factory_monitor_task
 * ================================================================ */

static void factory_monitor_task(void *argument)
{
    (void)argument;
    dev_display_t *dsp = dev_display_get();

    for (;;) {
    idle_restart:
        /* ===== IDLE: 等待 TEST 激活（业务数据中止 → 重新 IDLE） ===== */
        s_factory_abort  = false; /* 进入检测序列前清中止标志 */
        s_factory_active = false; /* 回 IDLE 即退出激活态 */
        if (!_factory_wait_tst(osWaitForever))
            continue;
        s_factory_active = true; /* 进入检测序列：激活态（占用显示资源） */

        /* ===== 第1次按键 → SHOW_CODE: 显示固件/模组信息 ===== */
        {
            const char *module_code = (dsp && dsp->module_code) ? dsp->module_code : "UNKNOWN";
            char info_buf[64];
            snprintf(info_buf, sizeof(info_buf), "FW:%s\nMD:%s", PROGRAM_CODE, module_code);

            dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
            app_render(&(render_cfg_t){
                .type      = RENDER_TEXT,
                .x         = 0,
                .y         = 0,
                .w         = dsp->screen_rows,
                .h         = dsp->screen_cols,
                .color     = COLOR_GREEN,
                .text      = info_buf,
                .len       = strlen(info_buf),
                .font_size = FONT_SELF_ADAPT,
                .font_type = FONT_ST,
                .text_enc  = FONT_ENC_UTF8,
                .style     = &(render_style_t){
                        .h_align = ALIGN_CENTER,
                        .v_align = ALIGN_CENTER,
                },
            });
        }

        /* ===== 第2次按键 → RED: 全屏红色 =====   */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_RED);
        dev_display_commit_frame(dsp);

        /* ===== 第3次按键 → GREEN: 全屏绿色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_GREEN);
        dev_display_commit_frame(dsp);

        /* ===== 第4次按键 → YELLOW: 全屏黄色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_YELLOW);
        dev_display_commit_frame(dsp);

        /* ===== 第5次按键 → WHITE: 全屏白色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_WHITE);
        dev_display_commit_frame(dsp);

        /* ===== 第6次按键 → BLUE: 全屏蓝色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLUE);
        dev_display_commit_frame(dsp);

        /* ===== 第7次按键 → PURPLE: 全屏紫色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_PURPLE);
        dev_display_commit_frame(dsp);

        /* ===== 第8次按键 → CYAN: 全屏青色 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_CYAN);
        dev_display_commit_frame(dsp);

        /* ===== 第9次按键 → 通信灯红叉 + 报警灯开启 ===== */
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
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
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
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
        if (!_factory_wait_tst(osWaitForever))
            goto idle_restart;
        osThreadSuspend(g_light_sensor_task_handle);
        dev_display_set_brightness(dsp, DEV_DISPLAY_BRIGHTNESS_MAX);
        dev_display_fill(dsp, 0, 0, dsp->screen_rows, dsp->screen_cols, COLOR_BLACK);
        dev_display_commit_frame(dsp);

        bool aging_exit = false;
        bool lane_green = true;
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
                    dev_io_lane_light(lane_green);
                    lane_green = !lane_green;

                    bool pressed = _factory_wait_tst(3000);
                    if (pressed || s_factory_abort) {
                        aging_exit = true;
                        break;
                    }
                    ch_ptr += ch_len;
                }
                if (aging_exit) break;
            }
            if (aging_exit) break;
        }

        /* 老化退出（含业务数据中止）→ 恢复光敏自动调光 */
        osThreadResume(g_light_sensor_task_handle);

        /* ===== 第12次按键 → OBLIQUE_SCAN: 斜扫模式 ===== */
        app_test_oblique_scan();

        /* ===== 第13次按键 → 重启程序 ===== */
        NVIC_SystemReset();
    }
}

/* ---- 模块自注册 ---- */
static void _factory_test_init(void)
{
    const osThreadAttr_t attr = {
        .name       = "factory_monitor",
        .stack_size = 256 * 4, /* 再压一档；原 2KB/4KB 过大，多协议下挤堆 */
        .priority   = osPriorityHigh,
    };
    g_factory_test = osThreadNew(factory_monitor_task, NULL, &attr);
}
sw_app_initcall(_factory_test_init);

/* 收到业务数据时中止工厂监控当前序列（置中止标志，monitor 任务回 IDLE 重新待机）。
 * 不销毁任务：TEST 键出厂检测/老化入口必须随时可用。
 * 业务数据到达仅中止当前检测序列，按键扫描持续运行。 */
void app_factory_mode_interrupt(void)
{
    s_factory_abort = true;
}
