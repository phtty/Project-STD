/**
 * @file    dev_display.c
 * @brief   HUB75 显示设备 — 通用 scan_task 骨架 + ISR 回调
 *
 * 派生的模组类型（P16、P10、静态等）通过 dev_display_ops 注入差异。
 * scan_task 负责调度，不被任何其他任务抢占（osPriorityRealtime）。
 */

#include "dev_display.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_tim.h"

/* ---- 扫描任务事件 ---- */
static osEventFlagsId_t s_scan_evt;
static dev_display_t *s_active_display;
static uint8_t *s_frame_front;
static uint8_t *s_frame_back;
static volatile uint8_t *s_pending_frame;
static volatile uint8_t s_frame_ready;

volatile uint32_t g_dev_display_commit_count;
volatile uint32_t g_dev_display_scan_count;

/* ---- 实例注册（由派生模组的 hw_dev_initcall 调用）---- */
void dev_display_register(dev_display_t *dev) { s_active_display = dev; }

dev_display_t *dev_display_get(void) { return s_active_display; }

void dev_display_commit_frame(dev_display_t *dev)
{
    if (!dev)
        return;

    osKernelLock();
    s_pending_frame = dev->pixel_map;
    s_frame_ready   = 1U;
    g_dev_display_commit_count++;
    osKernelUnlock();
}

/* ---- TIM 周期回调（前向声明，实现在文件末尾）---- */
static void _on_tim3_period(void);
static void _on_tim4_period(void);

/* ---- 硬件初始化（所有模组通用）---- */
void dev_display_init(void)
{
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM3));
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM4));
    pl_hub75_init();
    pl_tim_register_period_cb(PL_TIM3, _on_tim3_period);
    pl_tim_register_period_cb(PL_TIM4, _on_tim4_period);
}
hw_dev_initcall(dev_display_init);

/* ---- 扫描任务骨架 ---- */
static void scan_task(void *arg)
{
    dev_display_t *dev = (dev_display_t *)arg;
    static uint8_t scan_line;

    pl_tim_start_it(pl_tim_get_handle(PL_TIM3));
    pl_tim_start_it(pl_tim_get_handle(PL_TIM4));

    s_frame_front = dev->pixel_map;
    s_frame_back  = dev->pixel_map;

    for (;;) {
        osEventFlagsWait(s_scan_evt, 0x01, osFlagsWaitAny, osWaitForever);

        /* 帧提交 → 预计算（off critical path） */
        osKernelLock();
        bool frame_ready = s_frame_ready != 0U;
        if (frame_ready) {
            s_frame_front = (uint8_t *)s_pending_frame;
            s_frame_back  = dev->hub75_buff;
            s_frame_ready = 0U;
        }
        osKernelUnlock();

        if (frame_ready) {
            dev->dirty = false;
            if (dev->ops->prepare)
                dev->ops->prepare(dev);
        }

        /* 模组专用扫描输出 */
        g_dev_display_scan_count++;
        dev->ops->scan(dev, scan_line);

        /* OE/LAT 原子窗口（所有模组通用） */
        osKernelLock();
        pl_tim_irq_disable(TIM4_IRQn);
        pl_hub75_oe_set(true);
        if (dev->ops->set_row)
            dev->ops->set_row(scan_line);
        pl_hub75_latch_pulse();
        pl_tim_irq_enable(TIM4_IRQn);
        osKernelUnlock();

        scan_line = (scan_line + 1) % dev->scan_lines;
    }
}

/* ---- 软件初始化（创建事件 + 扫描任务）---- */
void dev_display_start(void)
{
    s_scan_evt = osEventFlagsNew(NULL);

    dev_display_t *dev = s_active_display;

    dev->dirty = true;

    const osThreadAttr_t attr = {
        .name       = "scan_task",
        .stack_size = 256 * 4, /* 扫屏路径无大栈帧；原 2KB 偏大 */
        .priority   = osPriorityRealtime,
    };
    osThreadNew(scan_task, dev, &attr);
}
sw_dev_initcall(dev_display_start);

/* ---- 通用像素操作 ---- */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, display_color_t color)
{
    if (x < dev->screen_rows && y < dev->screen_cols) {
        dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
        dev->dirty                               = true;
    }
}

void dev_display_set_brightness(dev_display_t *dev, uint8_t level)
{
    if (level > DEV_DISPLAY_BRIGHTNESS_MAX) level = DEV_DISPLAY_BRIGHTNESS_MAX;
    dev->light_level = level;
}

void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t color)
{
    /* 起点越界：整区域不可见，直接丢弃。
     * 必须先判起点再截断——否则 screen_rows - x / screen_cols - y 无符号下溢成巨值，
     * memset/行循环写穿 pixel_map（CCMRAM）导致 HardFault。 */
    if (x >= dev->screen_rows || y >= dev->screen_cols)
        return;
    /* 截断判断用 32 位运算：右对齐下溢 cur_x≈0xFFF0 时，x+w 的 uint16 加法回绕成
     * 小值绕过判界，改为 (uint32_t)x + w 后正确识别越界并截断 */
    if ((uint32_t)x + w > dev->screen_rows) w = dev->screen_rows - x;
    if ((uint32_t)y + h > dev->screen_cols) h = dev->screen_cols - y;

    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    dev->dirty = true;
}

void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bitmap, display_color_t color)
{
    /* 起点越界早退（与 fill 对齐）：x+w / y+h 为 uint16 加法，
     * 起点已回绕出屏时 32 位判界也无法救回，直接丢弃整区域 */
    if (x >= dev->screen_rows || y >= dev->screen_cols)
        return;
    /* 越界判断用 32 位运算防 x+w / y+h 的 uint16 回绕绕过判界（根因同 fill） */
    if ((uint32_t)x + w > dev->screen_rows || (uint32_t)y + h > dev->screen_cols)
        return;

    uint16_t row_bytes = (w + 7) / 8;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            if (bitmap[row * row_bytes + col / 8] & (0x80 >> (col % 8))) {
                /* 索引中间值用 uint32 防 (y+row)*rows + (x+col) 的 uint16 回绕；
                 * 早退保证终值仍落在 pixel_map 内 */
                uint32_t idx = (uint32_t)(y + row) * dev->screen_rows + (uint32_t)(x + col);
                dev->pixel_map[idx] = (uint8_t)color;
            }
        }
    }
    dev->dirty = true;
}

/* ---- TIM 周期回调（通过 pl_tim_register_period_cb 注册到 Platform 层）---- */

static void _on_tim3_period(void)
{
    osEventFlagsSet(s_scan_evt, 0x01);
}

static void _on_tim4_period(void)
{
    dev_display_t *dev = dev_display_get();
    static uint8_t pwm_cnt;

    pl_hub75_oe_set(pwm_cnt >= dev->light_level);
    pwm_cnt = (pwm_cnt + 1) & 7;
}
