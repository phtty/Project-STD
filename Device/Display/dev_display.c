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

/* ---- 实例注册（由派生模组的 hw_dev_initcall 调用）---- */
void dev_display_register(dev_display_t *dev) { s_active_display = dev; }

dev_display_t *dev_display_get(void) { return s_active_display; }

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

    for (;;) {
        osEventFlagsWait(s_scan_evt, 0x01, osFlagsWaitAny, osWaitForever);

        /* 脏标记 → 预计算（off critical path） */
        if (dev->dirty) {
            dev->dirty = false;
            if (dev->ops->prepare)
                dev->ops->prepare(dev);
        }

        /* 模组专用扫描输出 */
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
        .stack_size = 512,
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
    if (level > 7) level = 7;
    dev->light_level = level;
}

void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, display_color_t color)
{
    if (x + w > dev->screen_rows) w = dev->screen_rows - x;
    if (y + h > dev->screen_cols) h = dev->screen_cols - y;

    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    dev->dirty = true;
}

void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bitmap, display_color_t color)
{
    if (x + w > dev->screen_rows || y + h > dev->screen_cols) return;

    uint16_t row_bytes = (w + 7) / 8;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            if (bitmap[row * row_bytes + col / 8] & (0x80 >> (col % 8)))
                dev->pixel_map[(y + row) * dev->screen_rows + (x + col)] = (uint8_t)color;
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
