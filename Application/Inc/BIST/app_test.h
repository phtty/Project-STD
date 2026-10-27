/**
 * @file    app_test.h
 * @brief   硬件测试用例 — 显示/渲染/IO 等功能验证
 *
 * 每个测试函数封装一个独立测试场景，可在 _init_task 中按需调用。
 * 正式发布时移除 app_test_run() 调用即可。
 */

#pragma once

/** @brief 运行全部测试用例 */
void app_test_run(void);

/** @brief 像素扫描测试：逐点亮绿灯再熄灭（持续循环，阻塞） */
void app_test_pixel_scan(void);

/** @brief 渲染测试：显示"通"字验证字库读取正常 */
void app_test_render_text(void);

/** @brief IO 测试：依次点亮车道灯/黄闪灯各 1 秒 */
/* 板级特有的 IO 测试（如车道灯/黄闪灯）不放这里：只有部分板有这些输出，
   声明见各板 boards/&lt;板&gt;/board.h。 */

/** @brief LED 灯序测试：逐通道逐像素点亮 hub75_buff，用于确认物理灯序映射 */
void app_test_led_mapping(void);

/** @brief 定点灯序测试：按固定顺序只亮几个关键移位位置，每个停 5 秒
 *
 *  比逐个走完 192 步快得多：用于核对"某一段/某一组的起点"到底落在屏幕哪里。
 *  当前顺序为 16 → 32 → 64 → 96 → 128 → 160 → 191（ch0）。 */
void app_test_led_points(void);

/** @brief 映射校验图：黑底 + 绿色边框 + 十字线 + 四角彩色标记
 *
 *  用于肉眼验证 prepare 的行列映射是否搞对：
 *    左上红、右上白、左下蓝、右下黄，边框与十字为绿。
 *  出现镜像/行错位/段错位时，四角颜色位置会立刻对不上。 */
void app_test_map_check(void);
