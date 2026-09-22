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
#include "app_cascade.h" /* 首次按键时认领主卡 */
#include "app_render.h"
#include "app_screen.h" /* app_screen_rows/cols：渲染要用**逻辑屏**几何，不是实屏 */
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
/** @brief 清屏（走**逻辑屏**：多卡时是整台设备，单卡时就是本卡）
 *
 *  **它必须与紧随其后的那次渲染一起**被 `dev_display_frame_begin/end` 包住 ——
 *  单独一次清屏会让屏上闪一帧全黑；包住之后只输出最终那一帧。
 *  两处调用点（SHOW_CODE、老化逐字）都是这个写法。 */
static void _clear_screen(void)
{
    app_render(&(render_cfg_t){
        .type  = RENDER_FILL,
        .x     = 0,
        .y     = 0,
        .color = COLOR_BLACK,
    });
}

static void _aging_fill_screen(font_size_t size, font_type_t type, const char *ch_utf8, uint8_t ch_len)
{
    /* **一律用逻辑屏几何**（`app_screen_rows/cols`），不用实屏（`dsp->screen_*`）：
       多卡时逻辑屏是整台设备（如 224×100），实屏只是本卡那半幅（224×50）。
       用实屏的话内容会落到整屏的左上角那一格 —— 也就是**从卡那一格**，
       于是"主卡按一下、整设备一起老化"变成"主卡自己黑屏、从卡显示内容"。

       `app_screen_rows/cols` 在门面停用（单卡、地址不在表里）时会回落到本卡屏几何，
       所以这段在哪种形态下都对。 */
    const uint16_t sw = app_screen_rows();
    const uint16_t sh = app_screen_cols();

    uint8_t cols = (uint8_t)(sh / size);
    uint8_t rows = (uint8_t)(sw / size);
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    /* 清屏也走逻辑屏（多卡主卡上"本卡实屏缓冲"与画布是两回事） ——
       与紧随的文字**当成一帧**输出，见 _clear_screen 的说明 */
    dev_display_frame_begin(dev_display_get());
    _clear_screen();

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
        .w         = sw,
        .h         = sh,
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
    dev_display_frame_end(dev_display_get());
}

/* ================================================================
 *  factory_monitor_task
 * ================================================================ */

static void factory_monitor_task(void *argument)
{
    (void)argument;

    for (;;) {
        /* IDLE: 等待 TEST 激活 */
        dev_key_wait_press(DEV_KEY_TST, osWaitForever);

        s_factory_active = true;

        /* ===== 主从识别（并入"第一次按键"这一步）=====
         * 用户定的口径：老化测试走 render API、而 render 已兼容级联，所以按主卡
         * 一个键就能触发整设备老化 —— **不必区分老化与主副卡识别**。
         *
         * **只投递请求**：写身份记录要几十毫秒、发识别帧要占 s_tx 并等 ACK（最长约
         * 1s）——那些必须由**级联任务**做（s_tx 与轮次都是它的）。本任务是
         * factory_monitor_task，而 TEST 键只有它一个消费者，在这里投递不会冲突。 */
        app_cascade_claim_master();

        /* ===== SHOW_CODE ===== */
        /* 清屏与文字**都走逻辑屏**（多卡时是整台设备的屏，单卡时就是本卡）
           —— 用实屏几何的话，内容会整块落到左上那一格（多卡时就是从卡那一格）。
           两步**当成一帧**输出：中间不让 scan_task 跑 prepare（见 dev_display.h）。 */
        dev_display_frame_begin(dev_display_get());
        _clear_screen();
        app_render(&(render_cfg_t){
            .type      = RENDER_TEXT,
            .x         = 0,
            .y         = 0,
            .w         = app_screen_rows(),
            .h         = app_screen_cols(),
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
        dev_display_frame_end(dev_display_get());

        dev_key_wait_press(DEV_KEY_TST, osWaitForever);

        /* ===== DEAD_PIXEL ===== */
        osThreadSuspend(g_light_sensor_task_handle);
        /* 亮度走**整屏**那个入口：`dev_display_set_brightness` 只设本卡实屏，
           从卡拿到的仍是每轮 IMAGE 里带的旧亮度 → 一块亮一块暗。
           `app_screen_set_brightness` 本地 + 置"待下发"，由级联广播给从卡。 */
        app_screen_set_brightness(7);
        for (uint8_t i = 0; i < DEAD_PIXEL_COLOR_COUNT; i++) {
            /* 颜色要**覆盖**：画布是 1bpp（只记亮/灭），颜色在协议里是逐卡给的
               （来自切分表）—— 不覆盖的话十种纯色会全显示成每块屏自己的那个颜色。
               覆盖之后两块屏一起按这个颜色亮（单卡就是本卡那块）。 */
            app_screen_set_color_override(s_dead_pixel_colors[i]);
            app_render(&(render_cfg_t){
                .type  = RENDER_FILL,
                .x     = 0,
                .y     = 0,
                .color = s_dead_pixel_colors[i],
            });
            dev_key_wait_press(DEV_KEY_TST, osWaitForever);
        }
        app_screen_set_color_override(0xFF); /* 取消覆盖：后面的老化轮播用各卡自己的颜色 */

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
                /* 字号能不能放下，判的是**逻辑屏**（多卡时整台设备更大，
                   大字在上面才排得开） */
                if (fsize > app_screen_rows() || fsize > app_screen_cols()) continue;

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

        /* 退出工厂模式 —— 同样清**逻辑屏**（多卡时两块一起清空） */
        s_factory_active = false;
        app_render(&(render_cfg_t){
            .type  = RENDER_FILL,
            .x     = 0,
            .y     = 0,
            .color = COLOR_BLACK,
        });
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
