/**
 * @file    dev_display.c
 * @brief   HUB75 显示设备 — 通用 _scan_task 骨架 + ISR 回调
 *
 * 派生的模组类型（P16、P10、静态等）通过 dev_display_ops 注入差异。
 * _scan_task 负责调度，不被任何其他任务抢占（osPriorityRealtime）。
 */

#include <stdio.h>

#include "dev_display.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_tim.h"
#include "pl_task.h"

/* ---- 扫描任务事件 ---- */
static osEventFlagsId_t s_scan_evt;
static dev_display_t *s_active_display;

/* ---- 实例注册（由派生模组的 hw_dev_initcall 调用）---- */
void dev_display_register(dev_display_t *dev)
{
    s_active_display = dev;
}

dev_display_t *dev_display_get(void)
{
    return s_active_display;
}

/* ---- 模组颜色能力查询 ----
 *
 * `supported_color_mask` 是**后加字段**：老驱动（以及任何漏填的指定初始化器）
 * 初始化后该成员为 0 —— 字面意思是"什么色都不支持"，那是错的默认。漏填必须
 * 保持旧行为（旧行为 = 8 色全通），所以 0 一律视为 DEV_DISPLAY_COLOR_ALL。
 * 有蓝灯珠的模组显式填掩码；只有 R/G 灯珠的 P20 填 0x0E（去掉 B/紫/青/白）。
 */
bool dev_display_supports_color(const dev_display_t *dev, dev_display_color_t c)
{
    if (dev == nullptr) return false;
    if (c == DEV_DISPLAY_COLOR_BLACK) return true;

    const uint8_t mask = (dev->supported_color_mask == 0U) ? DEV_DISPLAY_COLOR_ALL
                                                           : dev->supported_color_mask;
    return (mask & DEV_DISPLAY_COLOR_BIT(c)) != 0U;
}

uint8_t dev_display_color_mask(const dev_display_t *dev)
{
    if (dev == nullptr) return 0U;
    return (dev->supported_color_mask == 0U) ? DEV_DISPLAY_COLOR_ALL
                                             : dev->supported_color_mask;
}

/* ---- TIM 周期回调（前向声明，实现在文件末尾）---- */
static void _on_scan_period(void);
static void _on_pwm_period(void);

/* ---- 硬件初始化（所有模组通用）---- */
void dev_display_init(void)
{
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM_DISPLAY_SCAN));
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM_DISPLAY_PWM));
    pl_hub75_init();
    pl_tim_register_period_cb(PL_TIM_DISPLAY_SCAN, _on_scan_period);
    pl_tim_register_period_cb(PL_TIM_DISPLAY_PWM, _on_pwm_period);
}
hw_dev_initcall(dev_display_init);

/* ---- 扫描任务骨架 ---- */
static void _scan_task(void *arg)
{
    dev_display_t *dev = (dev_display_t *)arg;
    static uint8_t s_scan_line;

    pl_tim_start_it(pl_tim_get_handle(PL_TIM_DISPLAY_SCAN));
    pl_tim_start_it(pl_tim_get_handle(PL_TIM_DISPLAY_PWM));

    for (;;) {
        osEventFlagsWait(s_scan_evt, 0x01, osFlagsWaitAny, osWaitForever);

        /* 脏标记 → 预计算（off critical path）
         *
         * **只能在帧首做**（`s_scan_line == 0`）：`prepare` 重建的是**整屏**的扫描表
         * （P10 是 [扫行][时序步][端口] 三维表），而扫描是**逐行**输出的 ——
         * 在行间做，前一行用旧表、后一行用新表，屏上就是**一帧撕裂**（新旧各半）。
         * 现场表现：切换内容时屏幕"抖一下"，且抖的那一帧只出现在某些次更新上
         * （取决于更新落在这个 5ms 窗口里的哪一段），很难复现。
         * 挪到帧首之后，一帧要么全是旧的、要么全是新的；代价是更新最多晚一帧
         * （P10 一帧 10ms），肉眼看不出来。 */
        if (dev->dirty && s_scan_line == 0) {
            dev->dirty = false;
            if (dev->ops->prepare) {
                /* **量一下它到底多久**：这段跑在 osPriorityRealtime 的 _scan_task 里，
                   期间**所有 Normal 任务都上不来**（协议任务就在那一档）。
                   估算过一次（~10ms）与实测差一个数量级，所以直接量，不再估。
                   查完把这几行去掉。 */
                const uint32_t t0 = osKernelGetTickCount();
                dev->ops->prepare(dev);
                printf("[display] prepare 耗时 %u ms\n",
                       (unsigned)(osKernelGetTickCount() - t0));
            }
        }

        /* 模组专用扫描输出 */
        dev->ops->scan(dev, s_scan_line);

        /* OE/LAT 原子窗口（所有模组通用） */
        osKernelLock();
        pl_tim_irq_disable(pl_tim_irq_of(PL_TIM_DISPLAY_PWM));
        pl_hub75_oe_set(true); /* 消隐：行切换期间关断输出，避免鬼影 */
        if (dev->ops->set_row)
            dev->ops->set_row(s_scan_line);
        pl_hub75_latch_pulse();
        pl_tim_irq_enable(pl_tim_irq_of(PL_TIM_DISPLAY_PWM));
        osKernelUnlock();

        s_scan_line = (s_scan_line + 1) % dev->scan_lines;
    }
}

/* ---- 软件初始化（创建事件 + 扫描任务）---- */
void dev_display_start(void)
{
    s_scan_evt = osEventFlagsNew(NULL);

    dev_display_t *dev = s_active_display;

    dev->dirty = true;

    const osThreadAttr_t attr = {
        .name       = "_scan_task",
        .stack_size = 512,
        .priority   = osPriorityRealtime,
    };
    pl_task_new(_scan_task, dev, &attr);
}
sw_dev_initcall(dev_display_start);

/* ---- 通用像素操作 ---- */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color)
{
    if (!dev || x >= dev->screen_rows || y >= dev->screen_cols) return;
    dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
    if (!dev->dirty_hold) dev->dirty = true;
    else dev->frame_touched = true;
}

/* ---- 多步绘制当成一帧（见 dev_display.h 的说明）---- */

void dev_display_frame_begin(dev_display_t *dev)
{
    if (!dev) return;
    dev->dirty_hold    = true;
    dev->frame_touched = false;
}

void dev_display_frame_end(dev_display_t *dev)
{
    if (!dev) return;
    dev->dirty_hold = false;
    /* **真的写过实屏缓冲才输出**：画布路径整段不碰实屏缓冲，那一段就不该触发 prepare
       （多跑一次 prepare 在 P10 上是十几毫秒，本身就是一次看得见的刷新抖动）。 */
    if (dev->frame_touched) {
        dev->dirty         = true;
        dev->frame_touched = false;
    }
}

void dev_display_set_brightness(dev_display_t *dev, uint8_t level)
{
    if (level > 7) level = 7;
    dev->light_level = level;
}

void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t color)
{
    if (!dev) return;

    /* 起点已在屏外 → 整块不可见，直接返回。**必须先判**：否则 `screen_rows - x` 是负的 int，
       赋给 uint16_t 会回绕成巨大值，下面 memset 立刻冲出缓冲
       （生产上 LDI 四行文本在 3833024 上就能命中）。
       设备层**没有**"w/h = 0 表示全屏"的约定——0 尺寸就是什么都不画。 */
    if (x >= dev->screen_rows || y >= dev->screen_cols) return;

    /* 夹取用 32 位比较：防 x+w / y+h 在 uint16 上回绕后被误判成"未越界" */
    if ((uint32_t)x + w > dev->screen_rows) w = (uint16_t)(dev->screen_rows - x);
    if ((uint32_t)y + h > dev->screen_cols) h = (uint16_t)(dev->screen_cols - y);
    if (w == 0 || h == 0) return;

    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    if (!dev->dirty_hold) dev->dirty = true;
    else dev->frame_touched = true;
}

void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bitmap, dev_display_color_t color)
{
    if (!dev || !bitmap) return;
    if (x >= dev->screen_rows || y >= dev->screen_cols) return;

    /* 源 stride 用**裁剪前**的 w 算 —— 裁剪只砍可见列，不改变源位图的行字节布局。
       若先裁再算，行偏移会错位（右边缘花屏）。 */
    const uint16_t row_bytes = (uint16_t)((w + 7U) / 8U);

    /* 越界语义：由"整张放弃"改为**裁剪**，只画屏内可见部分 —— 与画布路径
       （app_screen_canvas.c 的 `_sink_bitmap`）逐位一致，契约要求两路同语义。 */
    if ((uint32_t)x + w > dev->screen_rows) w = (uint16_t)(dev->screen_rows - x);
    if ((uint32_t)y + h > dev->screen_cols) h = (uint16_t)(dev->screen_cols - y);
    if (w == 0 || h == 0) return;

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            if (bitmap[row * row_bytes + col / 8] & (0x80 >> (col % 8)))
                dev->pixel_map[(y + row) * dev->screen_rows + (x + col)] = (uint8_t)color;
        }
    }
    if (!dev->dirty_hold) dev->dirty = true;
    else dev->frame_touched = true;
}

/* ---- TIM 周期回调（通过 pl_tim_register_period_cb 注册到 Platform 层）---- */

static void _on_scan_period(void)
{
    osEventFlagsSet(s_scan_evt, 0x01);
}

static void _on_pwm_period(void)
{
    dev_display_t *dev = dev_display_get();
    static uint8_t s_pwm_cnt;

    pl_hub75_oe_set(s_pwm_cnt >= dev->light_level);
    s_pwm_cnt = (s_pwm_cnt + 1) & 7;
}
