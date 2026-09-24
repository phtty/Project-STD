/**
 * @file    dev_display_ref.h
 * @brief   test_screen_canvas / test_screen_layout 共用的 dev_display 参考实现
 *
 * **为什么提成共享头**：两个套件都需要"按真 `dev_display.c` 语义直写 1B/px
 * `pixel_map`"作为独立参考。两份曾经各自维护，其中一份漂移成了旧语义（`fill`
 * 只裁右下、`draw_bitmap` 越界**整张放弃**），而注释还自称"与另一份同一份" ——
 * 于是"参考"与"被测"对同一越界输入得出不同结果，对拍就失去意义。共享一份后
 * 再也没有第二处会漂移。
 *
 * 语义固定为**与真 `dev_display.c` 一致**（P1 修复后的裁剪语义）：
 *   · 指针为空 / 坐标越界（`x >= 宽` 或 `y >= 高`）整体忽略；
 *   · 超出屏幕的部分**裁剪**（只画可见部分），源位图 stride 仍按**裁剪前**的 w 算；
 *   · 写/叠加都置脏标记。
 *
 * **外部链接定义（不是 `static inline`）**：两个套件是各自独立的可执行文件，且各自
 * **只有一个 TU** 包含本头，故这里用外部链接定义（与它们原来各自写一份时一致）。
 * 不能用 `static inline`：`test/stubs/dev_display.h` 会先把这些名字声明成 extern，
 * `static` 定义在其后违反 C 的链接约定（"静态声明出现在非静态声明之后"）。
 * 每个可执行文件各得一份定义，不会符号冲突。`dev_display_get()` 与各套件的夹具
 * （`s_dev` 等）仍由套件自己提供。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dev_display.h"

void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color)
{
    if (!dev || x >= dev->screen_rows || y >= dev->screen_cols) return;
    dev->pixel_map[y * dev->screen_rows + x] = (uint8_t)color;
    dev->dirty                               = true;
}

void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      dev_display_color_t color)
{
    if (!dev) return;
    if (x >= dev->screen_rows || y >= dev->screen_cols) return;
    if ((uint32_t)x + w > dev->screen_rows) w = (uint16_t)(dev->screen_rows - x);
    if ((uint32_t)y + h > dev->screen_cols) h = (uint16_t)(dev->screen_cols - y);
    if (w == 0 || h == 0) return;
    for (uint16_t row = 0; row < h; row++)
        memset(&dev->pixel_map[(y + row) * dev->screen_rows + x], (uint8_t)color, w);
    dev->dirty = true;
}

void dev_display_draw_bitmap(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *bitmap, dev_display_color_t color)
{
    if (!dev || !bitmap) return;
    if (x >= dev->screen_rows || y >= dev->screen_cols) return;
    uint16_t row_bytes = (uint16_t)((w + 7) / 8); /* 裁剪前的 w */
    if ((uint32_t)x + w > dev->screen_rows) w = (uint16_t)(dev->screen_rows - x);
    if ((uint32_t)y + h > dev->screen_cols) h = (uint16_t)(dev->screen_cols - y);
    if (w == 0 || h == 0) return;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            if (bitmap[row * row_bytes + col / 8] & (0x80 >> (col % 8)))
                dev->pixel_map[(y + row) * dev->screen_rows + (x + col)] = (uint8_t)color;
    dev->dirty = true;
}

void dev_display_set_brightness(dev_display_t *dev, uint8_t level)
{
    if (!dev) return;
    if (level > 7) level = 7;
    dev->light_level = level;
}

/* ---- 多步绘制当成一帧：与生产同语义 ---- */
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
    if (dev->frame_touched) {
        dev->dirty         = true;
        dev->frame_touched = false;
    }
}
