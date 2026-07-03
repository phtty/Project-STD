/**
 * @file    app_test.h
 * @brief   硬件测试用例 — 显示/渲染/IO 等功能验证
 *
 * 每个测试函数封装一个独立测试场景，可在 init_task 中按需调用。
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
void app_test_io_output(void);

/** @brief LED 灯序测试：逐通道逐像素点亮 hub75_buff，用于确认物理灯序映射 */
void app_test_led_mapping(void);
