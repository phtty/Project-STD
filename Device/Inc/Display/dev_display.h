/**
 * @file    dev_display.h
 * @brief   HUB75 LED 点阵显示设备 — OCP 虚表基类
 *
 * 基类提供通用参数和 _scan_task 调度骨架。
 * 派生类通过 ops 虚表注入模组差异：像素映射、行地址编码、扫描策略。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pl_hub75.h"

/* ---- 颜色（上层 API 使用，不与 HUB75 引脚耦合）---- */
/** @brief 显示颜色 */
typedef enum {
    DEV_DISPLAY_COLOR_BLACK  = 0, /**< 黑（灭）*/
    DEV_DISPLAY_COLOR_RED    = 1, /**< 红 */
    DEV_DISPLAY_COLOR_GREEN  = 2, /**< 绿 */
    DEV_DISPLAY_COLOR_YELLOW = 3, /**< 黄 */
    DEV_DISPLAY_COLOR_BLUE   = 4, /**< 蓝 */
    DEV_DISPLAY_COLOR_PURPLE = 5, /**< 紫 */
    DEV_DISPLAY_COLOR_CYAN   = 6, /**< 青 */
    DEV_DISPLAY_COLOR_WHITE  = 7, /**< 白 */
} dev_display_color_t;

/** @brief 除黑外全支持的颜色掩码：bit N = 支持 `DEV_DISPLAY_COLOR_<N>`
 *  （bit0 黑恒支持，掩码里不表示） */
#define DEV_DISPLAY_COLOR_ALL (0xFEU)

/**
 * @brief 由颜色枚举拼出掩码中的一位（供驱动声明模组能力用）
 * @param c 颜色枚举值
 * @return 只含该颜色对应位的掩码
 */
#define DEV_DISPLAY_COLOR_BIT(c) ((uint8_t)(1U << (c)))

/** @brief HUB75 显示设备（不透明句柄）*/
typedef struct dev_display dev_display_t;

/* ---- 操作虚表 ---- */
/** @brief 显示操作虚表 —— 注入模组差异 */
typedef struct dev_display_ops {
    void (*prepare)(dev_display_t *dev);           /**< 像素→发送缓存 (dirty 时调用) */
    void (*scan)(dev_display_t *dev, uint8_t line); /**< 输出一个扫描行 */
    void (*set_row)(uint8_t row);                  /**< ABCD 行地址编码 */
} dev_display_ops_t;

/* ---- 基类 (派生类必须将其放在第一个成员位置) ---- */

/** @brief HUB75 LED 点阵显示设备基类 */
struct dev_display {
    const dev_display_ops_t *ops;  /**< 第一个成员：操作虚表 */

    /* 通用参数 */
    uint8_t  module_rows;         /**< 单模块像素行数 */
    uint8_t  module_cols;         /**< 单模块像素列数 */
    uint8_t  channels_per_module; /**< 每模块通道数 */
    uint8_t  modules_per_row;     /**< 每行模块数 */
    uint8_t  modules_per_col;     /**< 每列模块数 */
    uint8_t  scan_lines;          /**< 扫描行数 (静态=1, 1/4扫=4...) */
    /** 模组物理色彩能力：bit N = 支持 `DEV_DISPLAY_COLOR_<N>`；
     *  bit0（黑）恒支持，掩码里不表示。0 视为未填（全彩），见查询 API */
    uint8_t  supported_color_mask;

    /* 派生参数 */
    uint16_t screen_rows;         /**< = modules_per_row * module_rows */
    uint16_t screen_cols;         /**< = modules_per_col * module_cols */
    uint8_t  total_channels;      /**< = modules_per_col * channels_per_module */
    uint16_t channel_pixels;      /**< = module_rows * module_cols * modules_per_row / total_channels */
    uint16_t scan_line_pixels;    /**< = channel_pixels / scan_lines */
    uint16_t buffer_size;         /**< = screen_rows * screen_cols */

    /* 缓冲区 (CCMRAM，派生实例静态分配) */
    uint8_t *pixel_map;           /**< 像素颜色缓冲 */
    uint8_t *hub75_buff;          /**< HUB75 发送缓冲 */

    /* 运行时 */
    volatile uint8_t light_level; /**< 亮度等级 (0~7) */
    volatile bool    dirty;       /**< 有改动待输出 */
    /** @brief 见 `dev_display_frame_begin()`：这一段绘制当成一帧，期间不置脏标记 */
    bool dirty_hold;
    /** @brief 压制期间**真的写过实屏缓冲**没有 —— `frame_end` 据此决定要不要输出
     *  （画布路径整段都不碰实屏缓冲，那一段就不该触发 prepare） */
    bool frame_touched;
};

/* ---- 通用 API ---- */

/** @brief 硬件初始化 (hw_dev_initcall): HUB75 引脚 + DBG 冻结 */
void dev_display_init(void);

/** @brief 软件初始化 (sw_dev_initcall): 创建 _scan_task + 启动 TIM3/4 */
void dev_display_start(void);

/** @brief 设置单个像素颜色，置脏标记
 *  坐标越界（含 dev 为 nullptr）时直接忽略，不置脏标记
 *  @param[in,out] dev 目标显示设备
 *  @param x     像素列坐标
 *  @param y     像素行坐标
 *  @param color 目标颜色 */
void dev_display_set_pixel(dev_display_t *dev, uint16_t x, uint16_t y, dev_display_color_t color);

/** @brief 矩形区域填充纯色: (x,y)起点, w宽h高, 超出屏幕的部分自动裁剪（只画可见部分）
 *
 *  无"w/h = 0 表示全屏"的约定——0 尺寸什么都不画。置脏标记。
 *  @param[in,out] dev 目标显示设备
 *  @param x     矩形左上角列坐标
 *  @param y     矩形左上角行坐标
 *  @param w     矩形宽度
 *  @param h     矩形高度
 *  @param color 填充颜色 */
void dev_display_fill(dev_display_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h, dev_display_color_t color);

/** @brief 叠加绘制位图: (x,y)起点, w宽h高, bitmap每行( (w+7)/8 )字节, bit=1写color, bit=0不改变原像素。
 *  超出屏幕的部分自动裁剪（只画屏内可见部分）。
 *  源位图行字节恒按**传入的 w** 算（裁剪只砍可见列，不改变源布局）。
 *  如需不透明绘制(bit=0置黑), 调用方先 dev_display_fill 填充背景色
 *  @param[in,out] dev 目标显示设备
 *  @param x      位图左上角列坐标
 *  @param y      位图左上角行坐标
 *  @param w      位图宽度
 *  @param h      位图高度
 *  @param bitmap 1bpp 位图，每行 (w+7)/8 字节、行优先、MSB-first
 *  @param color  bit=1 处写入的颜色 */
void dev_display_draw_bitmap(dev_display_t *dev,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h,
    const uint8_t *bitmap, dev_display_color_t color);

/** @brief 查询模组是否支持某颜色
 *
 *  bit N = 支持 `DEV_DISPLAY_COLOR_<N>`；黑恒支持。
 *  `supported_color_mask` 未填（=0）视为全彩 —— 见实现处说明。
 *  @param dev 目标显示设备；nullptr 返回 false
 *  @param c   待查询的颜色
 *  @return true 支持；false 不支持或 dev 为 nullptr */
bool dev_display_supports_color(const dev_display_t *dev, dev_display_color_t c);

/** @brief 取模组的整张颜色能力掩码（供"按能力枚举可用色"用）
 *
 *  `supported_color_mask` 未填（=0）视为 `DEV_DISPLAY_COLOR_ALL` —— 见实现处说明。
 *  @param dev 目标显示设备；nullptr 返回 0
 *  @return 掩码：bit N = 支持 `DEV_DISPLAY_COLOR_<N>`（bit0 黑不表示） */
uint8_t dev_display_color_mask(const dev_display_t *dev);

/** @brief 获取 P20 模组显示实例
 *  @return 实例指针；未注册时返回 nullptr */
dev_display_t *dev_display_p20_get(void);

/** @brief 注册活动显示实例（由显示模组的 hw_dev_initcall 调用） */
void dev_display_register(dev_display_t *dev);

/** @brief 获取当前活动显示实例
 *  @return 活动实例指针；未注册时返回 nullptr */
dev_display_t *dev_display_get(void);

/** @brief 设置亮度 (0=最暗/关闭, 7=最亮)，PWM 粒度 1/8
 *  @param[in,out] dev 目标显示设备
 *  @param level 亮度等级 (0~7) */
void dev_display_set_brightness(dev_display_t *dev, uint8_t level);

/** @brief 把接下来的若干次绘制**当成一帧**输出（期间不置脏标记，`_end` 时置一次）
 *
 *  为什么需要它：多步更新（清背景 + 画内容）之间，`_scan_task` 是
 *  `osPriorityRealtime`，完全可能在两步之间跑一次 `prepare` —— 屏上就闪出中间态
 *  （一帧全黑、或半张新半张旧）。压住脏标记之后，屏上只出现最终那一帧。
 *
 *  **画布路径**（多卡主卡，渲染目标是 1bpp 画布）整段都不碰实屏缓冲 —— 那种情况下
 *  `_end` 不会置脏标记（`frame_touched` 记着），不会白跑一次 prepare。
 *
 *  **必须成对**：忘了 `_end` 的表现是"这一段更新一直不上屏"，不报错。
 *  与落屏路径原来那句 `dev->dirty = false` 是同一件事，这里提成显式接口 ——
 *  上层不必再去摸 `dirty`。
 *  @param[in,out] dev 目标显示设备 */
void dev_display_frame_begin(dev_display_t *dev);

/** @brief 结束"当成一帧"的压制：按需置脏标记并让 _scan_task 输出
 *  @param[in,out] dev 目标显示设备 */
void dev_display_frame_end(dev_display_t *dev);
