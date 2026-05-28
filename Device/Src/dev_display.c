/**
 * @file    dev_display.c
 * @brief   HUB75 LED 点阵显示设备实现
 *
 * 像素缓冲区在 CCMRAM 中静态分配。
 * 扫描引擎在 TIM3 ISR 中运行，亮度 PWM 在 TIM4 ISR 中运行。
 */

#include "dev_display.h"

#include <string.h>
#include "initcall.h"
#include "pl_tim.h"

/* ---- CCMRAM 缓冲区 ---- */
[[gnu::section(".ccmram")]] static uint8_t s_pixel_map[DISRAM_SIZE];
[[gnu::section(".ccmram")]] static uint8_t s_hub75_buff[DISRAM_SIZE];

/* ---- 全局显示设备实例 ---- */
static display_dev_t g_display = {
    .pixel_map   = s_pixel_map,
    .hub75_buff  = s_hub75_buff,
    .light_level = 1,
};

display_dev_t *dev_display_get(void)
{ return &g_display; }

/* ---- 初始化 ---- */
void dev_display_init(void)
{
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM3));
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM4));

    pl_hub75_init();
    dev_display_fill(&g_display, HUB75_COLOR_BLACK);
    pl_tim_start_it(pl_tim_get_handle(PL_TIM3));
    pl_tim_start_it(pl_tim_get_handle(PL_TIM4));
}
hw_device_initcall(dev_display_init);

/* ---- 像素操作 ---- */
void dev_display_set_pixel(display_dev_t *dev, uint16_t x, uint16_t y, hub75_color_t color)
{
    if (x < SCREEN_PIXEL_ROW && y < SCREEN_PIXEL_COL)
        dev->pixel_map[y * SCREEN_PIXEL_ROW + x] = (uint8_t)color;
}

void dev_display_fill(display_dev_t *dev, hub75_color_t color)
{
    memset(dev->pixel_map, (uint8_t)color, DISRAM_SIZE);
}

/* ---- 像素图转换（pixel_map → hub75_buff） ---- */
void dev_display_convert(display_dev_t *dev)
{
    uint16_t group_cnt = 0, group_row = 0, group_col = 0, row_cnt = 0, col_cnt = 0;

    for (uint16_t map_cnt = 0; map_cnt < DISRAM_SIZE; map_cnt++) {
        row_cnt = map_cnt / SCREEN_PIXEL_ROW;
        col_cnt = map_cnt % SCREEN_PIXEL_ROW;

        group_row = (row_cnt / 4) ^ 1;
        group_col = col_cnt / 4;

        group_cnt = (4 * ((group_row + 1) / 2) + group_row / 2 * (CHANNEL_PIXEL_NUM / 16 - 4)) + (group_col + group_col / 4 * 4);

        switch (row_cnt % 4) {
            case 0:
                dev->hub75_buff[1 * 4 + (col_cnt % 4) + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 1:
                dev->hub75_buff[0 * 4 + (col_cnt % 4) + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 2:
                dev->hub75_buff[3 * 4 + (col_cnt % 4) + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
            case 3:
                dev->hub75_buff[2 * 4 + (col_cnt % 4) + group_cnt * GROUP_SIZE] = dev->pixel_map[map_cnt];
                break;
        }
    }
}

/* ---- HAL 周期回调（__weak 覆盖）---- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        dev_display_tim3_isr(&g_display);
    } else if (htim->Instance == TIM4) {
        dev_display_tim4_isr(&g_display);
    }
    if (htim->Instance == TIM7) {
        HAL_IncTick(); /* 系统时基 */
    }
}

/* ---- TIM3 ISR: HUB75 扫描行输出 ---- */
#define LINE_OFFSET    (scan_line * SCAN_LINE_PIXEL_NUM)
#define CHANNEL_OFFSET (channel_cnt * CHANNEL_PIXEL_NUM)

void dev_display_tim3_isr(display_dev_t *dev)
{
    static uint8_t scan_line = 0;

    for (int16_t line_cnt = 0; line_cnt < SCAN_LINE_PIXEL_NUM; line_cnt++) {
        for (int16_t channel_cnt = 0; channel_cnt < CHANNEL_NUM; channel_cnt++) {
            uint8_t color = dev->hub75_buff[line_cnt + LINE_OFFSET + CHANNEL_OFFSET];
            pl_hub75_set_rgb(channel_cnt, (hub75_color_t)color);
        }

        pl_hub75_clock_pulse();
    }

    pl_tim_irq_disable(TIM4_IRQn);
    pl_hub75_oe_set(true);
    pl_hub75_set_row(scan_line);
    pl_hub75_latch_pulse();
    pl_tim_irq_enable(TIM4_IRQn);

    scan_line += 1;
    if (scan_line >= MODULE_SCAN_LINE_NUM)
        scan_line = 0;
}

/* ---- TIM4 ISR: PWM 亮度控制 ---- */
void dev_display_tim4_isr(display_dev_t *dev)
{
    static uint8_t pwm_cnt = 0;

    if (pwm_cnt < dev->light_level)
        pl_hub75_oe_set(false);
    else
        pl_hub75_oe_set(true);

    pwm_cnt++;
    if (pwm_cnt >= 8)
        pwm_cnt = 0;
}
