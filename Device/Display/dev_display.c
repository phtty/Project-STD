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

/* ---- BSRR 预计算查表 (CCMRAM，零等待) ---- */
typedef struct {
    uint32_t r_val, g_val, b_val;
    GPIO_TypeDef *r_port, *g_port, *b_port;
} hub75_bsrr_t;

[[gnu::section(".ccmram")]] static hub75_bsrr_t g_bsrr[CHANNEL_NUM][8]; /* 8通道 × 8颜色 */

/* ---- 全局显示设备实例 ---- */
static display_dev_t g_display = {
    .pixel_map   = s_pixel_map,
    .hub75_buff  = s_hub75_buff,
    .light_level = 1,
};

display_dev_t *dev_display_get(void)
{
    return &g_display;
}

/* ---- 初始化 ---- */
void dev_display_init(void)
{
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM3));
    pl_tim_dbg_freeze(pl_tim_get_handle(PL_TIM4));

    pl_hub75_init();

    /* 预计算 BSRR 查表：消除 ISR 热路径的 if/else 分支和 Flash 读取 */
    for (uint8_t ch = 0; ch < CHANNEL_NUM; ch++) {
        for (uint8_t c = 0; c < 8; c++) {
            g_bsrr[ch][c].r_port = g_hub75_pin_r[ch].port;
            g_bsrr[ch][c].r_val  = (c & 1) ? (uint32_t)g_hub75_pin_r[ch].pin
                                           : (uint32_t)g_hub75_pin_r[ch].pin << 16;
            g_bsrr[ch][c].g_port = g_hub75_pin_g[ch].port;
            g_bsrr[ch][c].g_val  = (c & 2) ? (uint32_t)g_hub75_pin_g[ch].pin
                                           : (uint32_t)g_hub75_pin_g[ch].pin << 16;
            g_bsrr[ch][c].b_port = g_hub75_pin_b[ch].port;
            g_bsrr[ch][c].b_val  = (c & 4) ? (uint32_t)g_hub75_pin_b[ch].pin
                                           : (uint32_t)g_hub75_pin_b[ch].pin << 16;
        }
    }

    dev_display_fill(&g_display, HUB75_COLOR_BLACK);
    pl_tim_start_it(pl_tim_get_handle(PL_TIM3));
    pl_tim_start_it(pl_tim_get_handle(PL_TIM4));
}
hw_dev_initcall(dev_display_init);

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

/* ---- 前向声明（热路径，编译期内联） ---- */
static inline void tim3_scan_isr(void);
static inline void tim4_pwm_isr(void);

/* ---- HAL 周期回调（__weak 覆盖）---- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        tim3_scan_isr();

    } else if (htim->Instance == TIM4) {
        tim4_pwm_isr();

    } else if (htim->Instance == TIM7) {
        HAL_IncTick();
    }
}

/* ---- TIM3 ISR: HUB75 扫描行输出 ---- */
static inline void tim3_scan_isr(void)
{
    static uint8_t scan_line;

    for (uint16_t l = 0; l < SCAN_LINE_PIXEL_NUM; l++) {
        uint16_t base = (uint16_t)scan_line * SCAN_LINE_PIXEL_NUM + l;
        for (uint8_t ch = 0; ch < CHANNEL_NUM; ch++) {
            hub75_bsrr_t *b = &g_bsrr[ch][s_hub75_buff[base + ch * CHANNEL_PIXEL_NUM]];
            b->r_port->BSRR = b->r_val;
            b->g_port->BSRR = b->g_val;
            b->b_port->BSRR = b->b_val;
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
static inline void tim4_pwm_isr(void)
{
    static uint8_t pwm_cnt;

    pl_hub75_oe_set(pwm_cnt >= g_display.light_level);

    pwm_cnt = (pwm_cnt + 1) & 7; /* mod 8 */
}
