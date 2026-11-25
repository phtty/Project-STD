#pragma once

/**
 * @file    app_p25rls_pic.h
 * @brief   P25RLS 图片槽 —— 3 个全屏双色平面位图槽（协议图片编号 0/1/2）+ 持久化
 *
 * 协议只有 `SV`（显示并保存）**没有**"显示已存储图片"的命令，所以槽的作用只有一个：
 * **断电保存** —— `SV` 写进槽，上电由本模块重放最后显示的那一槽。
 *
 * **为什么不用渲染层的持久化（`app_render` 的 persist）**：那份记录是"1bpp + 一个
 * 颜色"，而 P25 的位图是红/绿两个平面、同屏三色（红/绿/黄），存进去会塌成单色。
 * 所以本模块自己存原始双平面位图，上电按同样三遍重放。
 *
 * **存储**：整份记录（有效位 + 最后显示槽 + 3 槽位图）作为**一条** `app_cfg_sched`
 * 记录（名字 `p25_pic`，版本 1），落在 W25Qxx 尾部配置区 —— 地址由调度器按名字
 * 反查分配，本模块不接触绝对地址。
 *
 * **RAM 镜像必须留在 SRAM**：`s_rec` 是写 Flash 的源缓冲，W25Qxx 走 SPI DMA，
 * 而 DMA 到不了 CCMRAM（与 RLS 图片槽同一个理由）。
 */

#include <stdint.h>
#include <stdbool.h>

#include "app_p25rls_cmd.h"

/** @brief 图片槽个数 —— 协议 `SV` 的"图片编号"取值 0/1/2 */
#define P25RLS_PIC_SLOT_COUNT (3U)

/** @brief 单槽记录：双平面 1bpp 位图（红平面 + 绿平面，与线上字节序一致） */
typedef struct {
    uint8_t bitmap[P25RLS_BITMAP_BYTES]; /**< 先红平面 288B、后绿平面 288B */
} p25rls_pic_slot_t;

/**
 * @brief 把一帧位图写进槽并持久化
 *
 * 只更新 RAM 镜像 + 落盘，**不显示**（显示由调用方随后调 `p25rls_pic_show()` 完成，
 * 这样"存"与"显示"的失败互不牵连）。落盘失败时 RAM 已更新，本次会话仍能显示。
 *
 * @param slot   槽号 0..P25RLS_PIC_SLOT_COUNT-1；越界直接返回 false
 * @param bitmap 576 字节双平面位图（调用方须已校验帧内有这么多字节）
 * @return true = 落盘成功；false = 参数非法或落盘失败（RAM 可能已更新）
 */
bool p25rls_pic_save(uint8_t slot, const uint8_t *bitmap);

/**
 * @brief 显示某个槽（先清屏，再按红/绿/黄三遍绘制）
 *
 * 三遍拆分的理由：渲染引擎的位图原语是"一张 1bpp 图 + 一个颜色"，而本协议的位图
 * 一个像素可能是红、绿或黄。三个掩码互不相交（红独有 / 绿独有 / 两平面都有），
 * 依次画就得到逐像素正确的三色图。
 *
 * **画布板（`BOARD_SCREEN_CANVAS=1`）会退化**：逻辑画布只记亮/灭，一帧里出现多种
 * 非黑颜色就判"混色"、落屏时回落成该卡的卡片色。P25 的目标产品是单卡三色屏
 * （画布关闭），不受影响；以后要在画布板上用多色图，需要给画布加多色表达。
 *
 * 槽未写过、或空指针时**只清屏**（不画垃圾）。
 *
 * @param slot 槽号 0..P25RLS_PIC_SLOT_COUNT-1；越界按"未写入"处理
 */
void p25rls_pic_show(uint8_t slot);

/**
 * @brief 上电重放最后一次 `show` 的槽（由 `app_boot` 的"上电画面接管"钩子调用）
 *
 * 时机：**必须**在 `app_default_display()` 的 `app_render_restore()` 之前。多色图存不进
 * `render_persist`（那份记录只有 1bpp + 单色），所以本模块自己存、自己重放，并通过
 * `app_render_set_boot_paint()` 告诉 `app_boot`"别再用持久化记录/兜底文案盖我的画面"。
 * 挂 `sw_post` 直接画是不够的 —— `app_default_display()` 在 `initcall_run_sw()` 之后跑。
 *
 * @return true = 画了某个槽（上电画面已被本模块接管）；false = 没有有效槽、没动屏
 */
bool p25rls_pic_restore(void);
